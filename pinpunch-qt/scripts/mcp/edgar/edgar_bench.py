"""
edgar_bench.py — Standalone warm-latency benchmark for the EDGAR MCP server.

Runs INDEPENDENT of Claude Code / Cowork. Pure Python + the `mcp` SDK
client over streamable-http transport. Zero LLM tokens consumed.

Prerequisites
─────────────
1. Server must be running in HTTP mode in another terminal:

       /usr/local/bin/python3.11 \
           /Users/venkatsairamkadari/PinpunchTerminal/pinpunch-qt/scripts/mcp/edgar/edgar_mcp_server.py \
           --transport http --port 18766

   Wait for the cold-start log line:
       [edgar_mcp_server] starting on http://127.0.0.1:18766/mcp

   (The 30-90s edgartools import happens BEFORE that line is printed,
   so once you see it the server is warm.)

2. Run the benchmark in THIS terminal:

       /usr/local/bin/python3.11 \
           /Users/venkatsairamkadari/PinpunchTerminal/pinpunch-qt/scripts/mcp/edgar/edgar_bench.py

Acceptance criteria
───────────────────
PASS  → each warm call < 3.0s, total < 15.0s
FAIL  → any single call ≥ 3.0s OR total ≥ 15.0s

A FAIL means edgartools is re-hydrating per call somewhere inside the
wrapper and we need to dig deeper before promoting to bot integration.
"""

from __future__ import annotations

import argparse
import asyncio
import sys
import time
from typing import Any

try:
    from mcp import ClientSession
    from mcp.client.streamable_http import streamablehttp_client
except ImportError as exc:
    sys.stderr.write(
        f"[edgar_bench] mcp SDK client not available: {exc}\n"
        "Install with: pip install --break-system-packages mcp\n"
    )
    sys.exit(2)


# ─────────────────────────────────────────────────────────────────────────────
# Benchmark plan — 4 calls, all 8-K. We dropped get_insider_summary on
# 2026-05-22 after discovering it's structurally slow: forms_insider.py
# calls filing.obj() per Form 4 inside a loop, each = fresh SEC HTTP fetch.
# That's an I/O budget issue, not a warm-up issue, and doesn't belong in a
# warm-latency benchmark. Treat insider as a separate "background" tool;
# call it from cron jobs or pre-market warm-ups, not real-time research.
# ─────────────────────────────────────────────────────────────────────────────

CALLS: list[tuple[str, dict[str, Any]]] = [
    ("get_latest_8k",              {"ticker": "NVDA"}),
    ("get_latest_8k",              {"ticker": "TSLA"}),
    ("get_8k_events",              {"ticker": "AMD",  "limit": 5}),
    ("get_8k_events_categorized",  {"ticker": "AAPL", "limit": 10}),
]

# Acceptance thresholds (seconds)
THRESH_PER_CALL = 3.0
THRESH_TOTAL    = 15.0


# ─────────────────────────────────────────────────────────────────────────────
# Pretty output helpers — no dependencies
# ─────────────────────────────────────────────────────────────────────────────

def _trunc(text: str, n: int = 80) -> str:
    text = text.replace("\n", " ").strip()
    return text if len(text) <= n else text[: n - 1] + "…"


def _result_preview(result: Any) -> str:
    """Pull a short human-readable preview out of an MCP tool result."""
    try:
        content = getattr(result, "content", None) or []
        if not content:
            return "(empty result)"
        first = content[0]
        text = getattr(first, "text", None)
        if text:
            return _trunc(text, 80)
        return _trunc(repr(first), 80)
    except Exception as exc:  # pragma: no cover — defensive only
        return f"(preview failed: {exc})"


# ─────────────────────────────────────────────────────────────────────────────
# Core benchmark loop
# ─────────────────────────────────────────────────────────────────────────────

async def run_benchmark(url: str, verbose: bool) -> int:
    print(f"[edgar_bench] connecting to {url} ...", flush=True)

    t_connect_start = time.perf_counter()
    try:
        async with streamablehttp_client(url) as (read, write, _):
            async with ClientSession(read, write) as session:
                await session.initialize()
                t_connected = time.perf_counter()
                print(
                    f"[edgar_bench] connected + initialized in "
                    f"{t_connected - t_connect_start:.2f}s",
                    flush=True,
                )

                # List tools as a sanity check + a free warmup round-trip
                tools_resp = await session.list_tools()
                tool_names = sorted(t.name for t in tools_resp.tools)
                print(
                    f"[edgar_bench] server exposes {len(tool_names)} tools "
                    f"(first: {tool_names[0]}, last: {tool_names[-1]})",
                    flush=True,
                )

                # Verify all benchmark targets actually exist on the server
                missing = [name for name, _ in CALLS if name not in tool_names]
                if missing:
                    sys.stderr.write(
                        f"[edgar_bench] FAIL — server is missing these tools: {missing}\n"
                    )
                    return 2

                # ──────────────────────────────────────────────────────────────
                # Timed calls
                # ──────────────────────────────────────────────────────────────
                print()
                print("─" * 78)
                print(f"  {'#':<3} {'tool':<32} {'args':<22} {'elapsed':>10}")
                print("─" * 78)

                timings: list[tuple[str, float]] = []
                errors:  list[tuple[str, str]]   = []
                total_start = time.perf_counter()

                for idx, (tool_name, args) in enumerate(CALLS, start=1):
                    args_str = ", ".join(f"{k}={v!r}" for k, v in args.items())
                    t0 = time.perf_counter()
                    try:
                        result = await session.call_tool(tool_name, args)
                        elapsed = time.perf_counter() - t0
                        timings.append((tool_name, elapsed))
                        print(
                            f"  {idx:<3} {tool_name:<32} {_trunc(args_str, 22):<22} "
                            f"{elapsed:>8.2f}s",
                            flush=True,
                        )
                        if verbose:
                            print(f"        preview: {_result_preview(result)}")
                    except Exception as exc:
                        elapsed = time.perf_counter() - t0
                        errors.append((tool_name, str(exc)))
                        timings.append((tool_name, elapsed))
                        print(
                            f"  {idx:<3} {tool_name:<32} {_trunc(args_str, 22):<22} "
                            f"{elapsed:>8.2f}s  ERROR",
                            flush=True,
                        )
                        if verbose:
                            print(f"        error: {_trunc(str(exc), 200)}")

                total_elapsed = time.perf_counter() - total_start
                print("─" * 78)

                # ──────────────────────────────────────────────────────────────
                # Verdict
                # ──────────────────────────────────────────────────────────────
                per_call_max = max(t for _, t in timings) if timings else 0.0
                per_call_avg = sum(t for _, t in timings) / len(timings) if timings else 0.0

                print()
                print(f"  total elapsed : {total_elapsed:>6.2f}s  "
                      f"(threshold < {THRESH_TOTAL:.1f}s)")
                print(f"  max single   : {per_call_max:>6.2f}s  "
                      f"(threshold < {THRESH_PER_CALL:.1f}s)")
                print(f"  avg single   : {per_call_avg:>6.2f}s")
                if errors:
                    print(f"  errors       : {len(errors)} call(s) failed")
                    for name, msg in errors:
                        print(f"     - {name}: {_trunc(msg, 100)}")

                passed = (
                    not errors
                    and per_call_max < THRESH_PER_CALL
                    and total_elapsed < THRESH_TOTAL
                )
                print()
                print("  VERDICT      : " + ("PASS ✓" if passed else "FAIL ✗"))
                print()

                if not passed:
                    reasons: list[str] = []
                    if errors:
                        reasons.append(f"{len(errors)} call(s) errored")
                    if per_call_max >= THRESH_PER_CALL:
                        reasons.append(
                            f"max single call {per_call_max:.2f}s "
                            f">= {THRESH_PER_CALL:.1f}s threshold"
                        )
                    if total_elapsed >= THRESH_TOTAL:
                        reasons.append(
                            f"total {total_elapsed:.2f}s "
                            f">= {THRESH_TOTAL:.1f}s threshold"
                        )
                    print("  Why FAIL    : " + "; ".join(reasons))
                    print()
                    print("  Likely cause: edgartools may be re-hydrating its filings")
                    print("                index per tool call instead of staying warm.")
                    print("                Re-run with --verbose to see error details,")
                    print("                or run again — first benchmark after server boot")
                    print("                can include residual cold-start in call #1.")
                    return 1

                return 0

    except (ConnectionRefusedError, OSError) as exc:
        sys.stderr.write(
            f"[edgar_bench] cannot reach server at {url}: {exc}\n"
            "\n"
            "Is edgar_mcp_server.py running? Start it with:\n"
            "  /usr/local/bin/python3.11 \\\n"
            "    /Users/venkatsairamkadari/PinpunchTerminal/pinpunch-qt/scripts/mcp/edgar/edgar_mcp_server.py \\\n"
            "    --transport http --port 18766\n"
        )
        return 2
    except Exception as exc:
        sys.stderr.write(f"[edgar_bench] unexpected error: {type(exc).__name__}: {exc}\n")
        return 2


# ─────────────────────────────────────────────────────────────────────────────
# Entry point
# ─────────────────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Standalone warm-latency benchmark for the EDGAR MCP server.",
    )
    parser.add_argument(
        "--url",
        default="http://127.0.0.1:18766/mcp",
        help="Streamable-HTTP MCP endpoint (default: %(default)s)",
    )
    parser.add_argument(
        "-v", "--verbose",
        action="store_true",
        help="Print result preview / error detail for each call",
    )
    args = parser.parse_args()

    exit_code = asyncio.run(run_benchmark(args.url, args.verbose))
    sys.exit(exit_code)


if __name__ == "__main__":
    main()
