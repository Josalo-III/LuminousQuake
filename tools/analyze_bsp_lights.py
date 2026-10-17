#!/usr/bin/env python3
"""Diagnose raw-grid Quake light samples against BSP-solid visibility.

This is a read-only analysis aid. The light sample positions come from the
engine's --light-analysis diagnostic; BSP nodes and leaves come from the same
installed map. For static-world faces the tool models original LIGHT's
move-toward-center rule for an ordinary 16-unit sample grid. It does not infer
whether this BSP was compiled with extra samples or changed compiler settings,
and brush submodel faces need separate treatment. The error figures are
deliberately provisional and must not be used to calibrate runtime lights.
"""
import argparse
import csv
import json
import math
from pathlib import Path
import re
import struct
import subprocess


def pak_file(path, name):
    with path.open('rb') as stream:
        header = stream.read(12)
        if len(header) != 12:
            raise ValueError(f'{path}: short PAK header')
        magic, offset, length = struct.unpack('<4sii', header)
        if magic != b'PACK' or offset < 12 or length < 0 or length % 64:
            raise ValueError(f'{path}: invalid PAK directory')
        stream.seek(offset)
        directory = stream.read(length)
        if len(directory) != length:
            raise ValueError(f'{path}: truncated PAK directory')
        for entry in struct.iter_unpack('<56sii', directory):
            entry_name = entry[0].split(b'\0', 1)[0].decode('ascii')
            if entry_name.lower() == name.lower():
                start, size = entry[1:]
                if start < 12 or size < 0 or start + size > path.stat().st_size:
                    raise ValueError(f'{path}: invalid {name} extent')
                stream.seek(start)
                data = stream.read(size)
                if len(data) != size:
                    raise ValueError(f'{path}: truncated {name}')
                return data
    return None


def map_bytes(basedir, map_name):
    name = f'maps/{map_name}.bsp'
    loose = basedir / 'id1' / name
    if loose.is_file():
        return loose.read_bytes()
    for path in sorted((basedir / 'id1').glob('pak*.pak', case_sensitive=False), reverse=True):
        data = pak_file(path, name)
        if data is not None:
            return data
    raise FileNotFoundError(name)


class BrushTree:
    def __init__(self, bsp):
        if len(bsp) < 124 or struct.unpack_from('<i', bsp)[0] != 29:
            raise ValueError('expected a Quake BSP version 29')
        lumps = [struct.unpack_from('<ii', bsp, 4 + i * 8) for i in range(15)]

        def lump(index, stride):
            offset, size = lumps[index]
            if offset < 0 or size < 0 or offset + size > len(bsp) or size % stride:
                raise ValueError(f'invalid BSP lump {index}')
            return bsp[offset:offset + size]

        self.planes = [(row[:3], row[3]) for row in struct.iter_unpack('<4fi', lump(1, 20))]
        self.nodes = [(row[0], row[1:3]) for row in struct.iter_unpack('<i2h6h2H', lump(5, 24))]
        self.leaves = [row[0] for row in struct.iter_unpack('<ii6h2H4B', lump(10, 28))]
        self.vertices = list(struct.iter_unpack('<3f', lump(3, 12)))
        self.texinfo = list(struct.iter_unpack('<8f2i', lump(6, 40)))
        self.faces = list(struct.iter_unpack('<Hhihh4Bi', lump(7, 20)))
        self.edges = list(struct.iter_unpack('<2H', lump(12, 4)))
        self.surfedges = [row[0] for row in struct.iter_unpack('<i', lump(13, 4))]
        self.sample_frames = {}
        models = list(struct.iter_unpack('<9f7i', lump(14, 64)))
        if not models:
            raise ValueError('BSP has no world model')
        self.root = models[0][9]
        self.world_face_count = models[0][15]
        if not self.planes or not self.nodes or not self.leaves or not 0 <= self.root < len(self.nodes):
            raise ValueError('BSP has incomplete world hull')
        entity_text = lump(0, 1).decode('latin-1')
        self.entities = [dict(re.findall(r'"([^"\\]*)"\s*"([^"\\]*)"', block))
                         for block in re.findall(r'\{([^{}]*)\}', entity_text)]

    def contents(self, point):
        node = self.root
        for _ in range(len(self.nodes) + 1):
            if node < 0:
                leaf = -1 - node
                if not 0 <= leaf < len(self.leaves):
                    raise ValueError('BSP node references invalid leaf')
                return self.leaves[leaf]
            if node >= len(self.nodes):
                raise ValueError('BSP node references invalid node')
            plane, children = self.nodes[node]
            if not 0 <= plane < len(self.planes):
                raise ValueError('BSP node references invalid plane')
            normal, distance = self.planes[plane]
            side = sum(a * b for a, b in zip(point, normal)) - distance
            node = children[0 if side >= 0 else 1]
        raise ValueError('BSP node cycle')

    def clear(self, start, end):
        """Return whether the segment avoids solid world leaves (no entities)."""
        def visit(node, p0, p1, depth):
            if depth > len(self.nodes):
                raise ValueError('BSP node cycle')
            if node < 0:
                leaf = -1 - node
                if not 0 <= leaf < len(self.leaves):
                    raise ValueError('BSP node references invalid leaf')
                return self.leaves[leaf] != -2
            if node >= len(self.nodes):
                raise ValueError('BSP node references invalid node')
            plane, children = self.nodes[node]
            if not 0 <= plane < len(self.planes):
                raise ValueError('BSP node references invalid plane')
            normal, distance = self.planes[plane]
            d0 = sum(a * b for a, b in zip(p0, normal)) - distance
            d1 = sum(a * b for a, b in zip(p1, normal)) - distance
            if d0 >= 0 and d1 >= 0:
                return visit(children[0], p0, p1, depth + 1)
            if d0 < 0 and d1 < 0:
                return visit(children[1], p0, p1, depth + 1)
            fraction = d0 / (d0 - d1)
            middle = tuple(a + fraction * (b - a) for a, b in zip(p0, p1))
            near = 0 if d0 >= 0 else 1
            return (visit(children[near], p0, middle, depth + 1) and
                    visit(children[1 - near], middle, p1, depth + 1))

        return visit(self.root, start, end, 0)

    def sample_frame(self, face_index, normal):
        """Exact face texture bounds and tangent directions for LIGHT-style nudges."""
        if face_index in self.sample_frames:
            return self.sample_frames[face_index]
        face = self.faces[face_index]
        first, count, texture = face[2:5]
        if count < 3 or first < 0 or first + count > len(self.surfedges) or not 0 <= texture < len(self.texinfo):
            raise ValueError(f'invalid face {face_index}')
        axes = self.texinfo[texture]
        points = []
        for edge_index in self.surfedges[first:first + count]:
            if abs(edge_index) >= len(self.edges):
                raise ValueError(f'invalid edge in face {face_index}')
            vertex = self.edges[abs(edge_index)][0 if edge_index >= 0 else 1]
            if vertex >= len(self.vertices):
                raise ValueError(f'invalid vertex in face {face_index}')
            points.append(self.vertices[vertex])
        bounds = []
        for axis in (axes[:4], axes[4:8]):
            coords = [sum(a * b for a, b in zip(point, axis[:3])) + axis[3] for point in points]
            bounds.append((min(coords), max(coords)))
        s_axis, t_axis = axes[:3], axes[4:7]
        texture_normal = (t_axis[1]*s_axis[2] - t_axis[2]*s_axis[1],
                          t_axis[2]*s_axis[0] - t_axis[0]*s_axis[2],
                          t_axis[0]*s_axis[1] - t_axis[1]*s_axis[0])
        length = math.sqrt(sum(x*x for x in texture_normal))
        if length <= 0:
            raise ValueError(f'degenerate texture axes in face {face_index}')
        texture_normal = tuple(x / length for x in texture_normal)
        dot = sum(a*b for a, b in zip(texture_normal, normal))
        if abs(dot) < 1e-8:
            raise ValueError(f'perpendicular texture axes in face {face_index}')
        if dot < 0:
            texture_normal = tuple(-x for x in texture_normal)
            dot = -dot
        tangents = []
        for axis in (s_axis, t_axis):
            projection = sum(a*b for a, b in zip(axis, normal)) / dot
            squared = sum(x*x for x in axis)
            tangents.append(tuple((a - projection*b) / squared for a, b in zip(axis, texture_normal)))
        frame = (bounds, tangents)
        self.sample_frames[face_index] = frame
        return frame

    def adjusted_sample(self, face_index, grid_s, grid_t, raw, normal):
        """Mirror LIGHT's six face-center probes for an ordinary 16-unit grid."""
        bounds, tangents = self.sample_frame(face_index, normal)
        mids = [(low + high) * 0.5 for low, high in bounds]
        original = [math.floor(low / 16) * 16 + grid * 16
                    for (low, _), grid in zip(bounds, (grid_s, grid_t))]
        center = tuple(raw[i] + sum((mids[j] - original[j]) * tangents[j][i]
                                    for j in range(2)) for i in range(3))
        sample = original[:]
        for attempt in range(6):
            point = tuple(raw[i] + sum((sample[j] - original[j]) * tangents[j][i]
                                       for j in range(2)) for i in range(3))
            if self.clear(center, point):
                return point, attempt, True
            axis = 0 if attempt & 1 else 1
            sample[axis] += max(-8, min(8, mids[axis] - sample[axis]))
        # LIGHT applies one final eight-unit world-space move after its last
        # failed trace; this fallback was not itself tested for visibility.
        distance = math.dist(center, point)
        if distance:
            point = tuple(a + (b - a) * min(8 / distance, 1) for a, b in zip(point, center))
        return point, 6, False


def light_sources(entities, styles):
    sources = {}
    targets = {e.get('targetname'): e for e in entities if e.get('targetname')}
    for index, entity in enumerate(entities):
        if not entity.get('classname', '').startswith('light'):
            continue
        style = int(entity.get('style', '0'))
        if style not in styles:
            continue
        origin = tuple(float(x) for x in entity['origin'].split())
        if len(origin) != 3 or not all(map(math.isfinite, origin)):
            raise ValueError(f'bad origin for light {index}')
        scalar = float(entity.get('light', entity.get('_light', '300')))
        if scalar == 0:
            scalar = 300
        target = targets.get(entity.get('target'))
        direction = None
        if target and 'origin' in target:
            target_origin = tuple(float(x) for x in target['origin'].split())
            vector = tuple(a - b for a, b in zip(target_origin, origin))
            length = math.dist(target_origin, origin)
            if length:
                direction = tuple(x / length for x in vector)
        sources.setdefault(style, []).append((index, origin, scalar, direction))
    for style in styles:
        if len(sources.get(style, [])) != 1:
            raise ValueError(f'style {style} has {len(sources.get(style, []))} sources; expected one')
    return {style: sources[style][0] for style in styles}


def prediction(source, point, normal):
    _, origin, scalar, direction = source
    delta = tuple(a - b for a, b in zip(origin, point))
    distance = math.dist(origin, point)
    if distance <= 0 or distance >= scalar:
        return 0.0
    incoming = tuple(x / distance for x in delta)
    facing = sum(a * b for a, b in zip(normal, incoming))
    if facing <= 0:
        return 0.0
    if direction:
        outgoing = tuple(-x for x in incoming)
        if sum(a * b for a, b in zip(direction, outgoing)) < math.cos(math.radians(20)):
            return 0.0
    return (scalar - distance) * (0.5 + 0.5 * facing) * 0.5


def analyze(binary, basedir, map_name, styles):
    tree = BrushTree(map_bytes(basedir, map_name))
    sources = light_sources(tree.entities, styles)
    totals = {style: {'samples': 0, 'submodel_samples': 0, 'submodel_bright': 0,
                      'world_samples': 0, 'world_bright': 0, 'world_sample_in_solid': 0,
                      'world_bright_blocked_valid_endpoint': 0,
                      'world_squared_error_raw': 0.0, 'world_squared_error_visible': 0.0,
                      'world_adjusted': 0, 'world_adjustment_unresolved': 0,
                      'world_adjusted_in_solid': 0, 'world_adjusted_bright_blocked': 0,
                      'world_blocked_bright_faces': {},
                      'world_squared_error_adjusted_raw': 0.0,
                      'world_squared_error_adjusted_visible': 0.0,
                      'bright': 0, 'clear': 0, 'bright_clear': 0,
                      'bright_blocked': 0, 'bright_blocked_valid_endpoint': 0,
                      'source_in_solid': tree.contents(sources[style][1]) == -2,
                      'sample_in_solid': 0, 'bright_sample_in_solid': 0,
                      'valid_endpoint': 0, 'valid_endpoint_clear': 0,
                      'valid_endpoint_bright': 0,
                      'squared_error_raw': 0.0, 'squared_error_visible': 0.0,
                      'squared_error_raw_valid': 0.0, 'squared_error_visible_valid': 0.0}
              for style in styles}
    process = subprocess.Popen([str(binary), '-basedir', str(basedir), '-map', map_name,
                                '--light-analysis'], stdout=subprocess.PIPE, text=True)
    for line in process.stdout:
        if not line.startswith('LQ_LIGHT_SAMPLE,') or line.startswith('LQ_LIGHT_SAMPLE,map,'):
            continue
        fields = next(csv.reader([line]))
        style = int(fields[11])
        if style not in totals:
            continue
        face = int(fields[2])
        point = tuple(map(float, fields[5:8]))
        normal = tuple(map(float, fields[8:11]))
        observed = int(fields[12])
        data = totals[style]
        data['samples'] += 1
        bright = observed >= 8
        if face >= tree.world_face_count:
            data['submodel_samples'] += 1
            data['submodel_bright'] += bright
        else:
            data['world_samples'] += 1
            data['world_bright'] += bright
        data['bright'] += bright
        sample_solid = tree.contents(point) == -2
        data['sample_in_solid'] += sample_solid
        data['bright_sample_in_solid'] += bright and sample_solid
        if face < tree.world_face_count:
            data['world_sample_in_solid'] += sample_solid
        data['valid_endpoint'] += not sample_solid
        data['valid_endpoint_bright'] += bright and not sample_solid
        visible = tree.clear(sources[style][1], point)
        data['clear'] += visible
        data['valid_endpoint_clear'] += visible and not sample_solid
        data['bright_clear'] += bright and visible
        data['bright_blocked'] += bright and not visible
        data['bright_blocked_valid_endpoint'] += bright and not visible and not sample_solid
        if face < tree.world_face_count:
            data['world_bright_blocked_valid_endpoint'] += bright and not visible and not sample_solid
        raw = prediction(sources[style], point, normal)
        with_visibility = raw if visible else 0.0
        data['squared_error_raw'] += (min(255, raw) - observed) ** 2
        data['squared_error_visible'] += (min(255, with_visibility) - observed) ** 2
        if face < tree.world_face_count:
            data['world_squared_error_raw'] += (min(255, raw) - observed) ** 2
            data['world_squared_error_visible'] += (min(255, with_visibility) - observed) ** 2
            adjusted, attempts, located = tree.adjusted_sample(
                face, int(fields[3]), int(fields[4]), point, normal)
            data['world_adjusted'] += attempts > 0
            data['world_adjustment_unresolved'] += not located
            adjusted_solid = tree.contents(adjusted) == -2
            data['world_adjusted_in_solid'] += adjusted_solid
            adjusted_visible = tree.clear(sources[style][1], adjusted)
            data['world_adjusted_bright_blocked'] += bright and not adjusted_visible
            if bright and not adjusted_visible:
                key = str(face)
                data['world_blocked_bright_faces'][key] = data['world_blocked_bright_faces'].get(key, 0) + 1
            adjusted_raw = prediction(sources[style], adjusted, normal)
            adjusted_with_visibility = adjusted_raw if adjusted_visible else 0.0
            data['world_squared_error_adjusted_raw'] += (min(255, adjusted_raw) - observed) ** 2
            data['world_squared_error_adjusted_visible'] += (
                min(255, adjusted_with_visibility) - observed) ** 2
        if not sample_solid:
            data['squared_error_raw_valid'] += (min(255, raw) - observed) ** 2
            data['squared_error_visible_valid'] += (min(255, with_visibility) - observed) ** 2
    if process.wait() != 0:
        raise RuntimeError('--light-analysis failed')
    for style, data in totals.items():
        count = data['samples']
        data['source_entity'] = sources[style][0]
        data['rmse_raw'] = math.sqrt(data.pop('squared_error_raw') / count)
        data['rmse_visible'] = math.sqrt(data.pop('squared_error_visible') / count)
        valid = data['valid_endpoint']
        data['rmse_raw_valid'] = math.sqrt(data.pop('squared_error_raw_valid') / valid)
        data['rmse_visible_valid'] = math.sqrt(data.pop('squared_error_visible_valid') / valid)
        world = data['world_samples']
        data['world_rmse_raw'] = math.sqrt(data.pop('world_squared_error_raw') / world)
        data['world_rmse_visible'] = math.sqrt(data.pop('world_squared_error_visible') / world)
        data['world_rmse_adjusted_raw'] = math.sqrt(
            data.pop('world_squared_error_adjusted_raw') / world)
        data['world_rmse_adjusted_visible'] = math.sqrt(
            data.pop('world_squared_error_adjusted_visible') / world)
    return totals


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path, help='luminousquake-gpu executable')
    parser.add_argument('basedir', type=Path, help='Quake installation directory')
    parser.add_argument('--map', default='e1m3')
    parser.add_argument('--styles', type=int, nargs='+', default=[32, 33, 34])
    args = parser.parse_args()
    print(json.dumps(analyze(args.binary.resolve(), args.basedir.resolve(), args.map,
                             args.styles), indent=2))


if __name__ == '__main__':
    main()
