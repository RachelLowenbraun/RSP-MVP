#!/usr/bin/env python3
"""DEPRECATED — see audit_analyzer.py for the real F-I02 analyzer.

This stub existed in the first scaffold; the real implementation is in
audit_analyzer.py and takes both the JSONL evidence log AND the 240 fps
video recording. Its output is a full bench_validation_record with per-event
external verification.

Use:
    python3 scripts/audit_analyzer.py --jsonl <log.jsonl> --video <capture.mp4> --out <report.json>
"""
import sys
print(__doc__, file=sys.stderr)
sys.exit(2)
