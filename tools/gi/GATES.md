# GI light/volume gates (CPU, read-only)

Run against a checkout `<repo>`. The gates read `<repo>/project/scenes/Forest.json`; the bounds gate also reads
`<repo>/tools/gi/bounds_exceptions.json`, so a verdict is reproducible from a sha.

    python -I tools/gi/bounds_audit.py <repo> [--md out.md] [--json out.json]   # verdict: grep GATE
    python -I tools/gi/fixture_audit.py <repo>                                  # verdict: 'failing: 0'
    python -I tools/gi/wallcross.py <repo>                                      # verdict: last line (0 leak rows)
    python -I tools/gi/inv.py <repo> --md inv.md                                # Bounds hard-edge inventory; diff rows vs a baseline

Exceptions: every non-OK Bounds face must match one row (id, face, status) in `bounds_exceptions.json`. Add or remove the
rows in the SAME commit as the scene change they excuse, with the reason and who OK'd it. "listed but not seen" rows are stale:
prune them.
