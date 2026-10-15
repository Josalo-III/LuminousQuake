#!/usr/bin/env python3
"""Summarize sampled leaf functions from an xctrace time-profile XML export."""

import argparse
import collections
import xml.etree.ElementTree as ET


def main():
    parser = argparse.ArgumentParser(
        description="Summarize an XML export of xctrace's time-profile table")
    parser.add_argument("xml")
    parser.add_argument("--top", type=int, default=20)
    args = parser.parse_args()

    root = ET.parse(args.xml).getroot()
    binaries = {}
    for binary in root.iter("binary"):
        if binary.get("id") and binary.get("name"):
            binaries[binary.get("id")] = binary.get("name")

    frames = {}
    for frame in root.iter("frame"):
        identifier = frame.get("id")
        if not identifier or not frame.get("name"):
            continue
        binary = frame.find("binary")
        binary_name = "unknown"
        if binary is not None:
            binary_name = binary.get("name") or binaries.get(binary.get("ref"), "unknown")
        frames[identifier] = (frame.get("name"), binary_name)

    backtraces = {}
    for backtrace in root.iter("backtrace"):
        identifier = backtrace.get("id")
        if identifier:
            backtraces[identifier] = [
                frame.get("id") or frame.get("ref") for frame in backtrace.findall("frame")
            ]

    weights = {}
    for weight in root.iter("weight"):
        if weight.get("id") and weight.text:
            weights[weight.get("id")] = int(weight.text)

    functions = collections.Counter()
    project = collections.Counter()
    binary_totals = collections.Counter()
    total = 0
    for row in root.iter("row"):
        weight = row.find("weight")
        backtrace = row.find("backtrace")
        if weight is None or backtrace is None:
            continue
        stack = backtraces.get(backtrace.get("id") or backtrace.get("ref"), [])
        if not stack:
            continue
        sample_weight = int(weight.text) if weight.text else weights.get(weight.get("ref"), 0)
        if not sample_weight:
            continue
        function, binary = frames.get(stack[0], ("unknown", "unknown"))
        functions[(function, binary)] += sample_weight
        binary_totals[binary] += sample_weight
        if binary == "luminousquake":
            project[function] += sample_weight
        total += sample_weight

    def show(title, entries):
        print(title)
        for label, weight in entries:
            print(f"{weight / total * 100:6.2f}%  {weight / 1_000_000:9.3f} ms  {label}")

    if not total:
        raise SystemExit("No weighted time-profile samples found")
    show("Sampled CPU by binary", binary_totals.most_common(args.top))
    show("\nTop sampled leaf functions", [
        (f"{function} [{binary}]", weight)
        for (function, binary), weight in functions.most_common(args.top)
    ])
    show("\nTop LuminousQuake leaf functions", project.most_common(args.top))


if __name__ == "__main__":
    main()
