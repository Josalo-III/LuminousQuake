"""Small known-solid BSP tree for the read-only lighting diagnostic."""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from analyze_bsp_lights import BrushTree


tree = BrushTree.__new__(BrushTree)
tree.planes = [((1.0, 0.0, 0.0), 0.0)]
tree.nodes = [(0, (-2, -1))]  # x >= 0 is empty leaf 1; x < 0 is solid leaf 0.
tree.leaves = [-2, -1]
tree.root = 0

assert tree.contents((1, 0, 0)) == -1
assert tree.contents((-1, 0, 0)) == -2
assert tree.clear((2, 0, 0), (1, 0, 0))
assert not tree.clear((1, 0, 0), (-1, 0, 0))
assert not tree.clear((-1, 0, 0), (1, 0, 0))
assert not tree.clear((-2, 0, 0), (-1, 0, 0))

# A 16-unit lightmap grid can start outside a small face. Move the sample
# toward its face midpoint before comparing its stored light with ray results.
tree.planes = [((1.0, 0.0, 0.0), 2.0)]
tree.vertices = [(12, 0, 0), (28, 0, 0), (28, 16, 0), (12, 16, 0)]
tree.texinfo = [(1, 0, 0, 0, 0, 1, 0, 0, 0, 0)]
tree.faces = [(0, 0, 0, 4, 0, 0, 255, 255, 255, 0)]
tree.edges = [(0, 1), (1, 2), (2, 3), (3, 0)]
tree.surfedges = [0, 1, 2, 3]
tree.sample_frames = {}
adjusted, attempts, located = tree.adjusted_sample(0, 0, 0, (0, 0, 1), (0, 0, 1))
assert located and attempts > 0 and adjusted[0] > 2
assert tree.contents(adjusted) == -1
print('PASS BSP-solid segment visibility and leaf contents')
