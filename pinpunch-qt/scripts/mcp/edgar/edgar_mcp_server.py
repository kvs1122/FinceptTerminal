"""
edgar_mcp_server.py — Pinpunch EDGAR MCP server.

Exposes the SEC EDGAR module (mcp/edgar/*) as MCP tools for Claude Code
(or any MCP-compatible client). Imports edgartools ONCE at boot to amortize
the 30-90s cold-start cost across all queries in a session.

Tools exposed (28 total):
  8-K (5):       get_latest_8k, get_8k_events, get_8k_events_categorized,
                 get_8k_full_text, search_8k
  10-K (7):      get_latest_10k, extract_10k_sections, get_10k_full_text,
                 get_10k_markdown, search_10k, get_10k_exhibits, get_10k_metadata
  10-Q (6):      get_latest_10q, extract_10q_sections, get_10q_full_text,
                 get_10q_markdown, get_10q_metadata, search_10q
  Insider (3):   get_insider_transactions, get_insider_transactions_detailed,
                 get_insider_summary
  13-F (4):      get_13f_holdings, get_13f_top_holdings, get_13f_manager_info,
                 get_13f_summary
  Financials (3): get_financials, get_financial_metrics, get_xbrl_statements

Usage (stdio — for Claude Code / Cowork):
  python edgar_mcp_server.py --transport stdio

Usage (http — for testing):
  python edgar_mcp_server.py --transport http --port 18766

Environment:
  EDGAR_IDENTITY  — SEC User-Agent identity (e.g. "Your Name your@email.com")
                    Required by SEC fair-use rules. Defaults to a placeholder
                    if unset, but you should provide your real email.
"""

from __future__ import annotations

import logging
import os
import sys
from typing import Any, Optional, List

logger = logging.getLogger(__name__)

# ─────────────────────────────────────────────────────────────────────────────
# FastMCP availability check — MUST happen BEFORE we touch sys.path.
#
# WHY: Pinpunch's own `scripts/mcp/` directory shadows the installed `mcp`
# pip package. If we add scripts/ to sys.path first, `from mcp.server.fastmcp
# import FastMCP` resolves to Pinpunch's local `mcp/` dir (which has no
# `server` submodule) and quietly ImportErrors. So we load FastMCP from
# site-packages FIRST, while sys.path is still clean.
# ─────────────────────────────────────────────────────────────────────────────

MCP_SERVER_AVAILABLE = False
try:
    from mcp.server.fastmcp import FastMCP
    MCP_SERVER_AVAILABLE = True
except ImportError:
    try:
        from fastmcp import FastMCP  # type: ignore
        MCP_SERVER_AVAILABLE = True
    except ImportError:
        FastMCP = None  # type: ignore

# ─────────────────────────────────────────────────────────────────────────────
# Load Pinpunch's EDGAR submodules under a CUSTOM package name
# (`pinpunch_edgar.*`) using importlib, instead of going through
# `mcp.edgar.*`. This avoids the namespace collision with site-packages
# `mcp` entirely — sys.modules['mcp'] keeps pointing at the real pip
# package, so FastMCP and any other mcp.* lookups continue to work.
#
# The edgar modules use relative imports (`from .base import ...`), so we
# must load them as a real package: create a synthetic parent module
# `pinpunch_edgar` with the edgar dir on its __path__, then load each
# submodule with the dotted name `pinpunch_edgar.<name>` so __package__
# resolves correctly inside the relative-import machinery.
# ─────────────────────────────────────────────────────────────────────────────

import importlib.util
import types

_EDGAR_PKG_DIR = os.path.dirname(os.path.abspath(__file__))

_pinpunch_edgar_pkg = types.ModuleType("pinpunch_edgar")
_pinpunch_edgar_pkg.__path__ = [_EDGAR_PKG_DIR]  # type: ignore[attr-defined]
sys.modules["pinpunch_edgar"] = _pinpunch_edgar_pkg


def _load_pinpunch_edgar_module(short_name: str, filename: str):
    """Load <_EDGAR_PKG_DIR>/<filename> as `pinpunch_edgar.<short_name>`."""
    full_name = f"pinpunch_edgar.{short_name}"
    spec = importlib.util.spec_from_file_location(
        full_name,
        os.path.join(_EDGAR_PKG_DIR, filename),
    )
    if spec is None or spec.loader is None:
        raise ImportError(f"Cannot build spec for {filename}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[full_name] = module
    spec.loader.exec_module(module)
    return module

# ─────────────────────────────────────────────────────────────────────────────
# EDGAR cold-start — happens ONCE per server lifetime
# This is the whole point of this wrapper: amortize the 30-90s edgartools
# import cost across all queries served by this process.
# ─────────────────────────────────────────────────────────────────────────────

_EDGAR_DEFAULT_IDENTITY = "Pinpunch Research venkat.k0822@gmail.com"
EDGAR_IDENTITY = os.environ.get("EDGAR_IDENTITY", _EDGAR_DEFAULT_IDENTITY)

print(
    f"[edgar_mcp_server] cold-starting edgartools with identity '{EDGAR_IDENTITY}' "
    f"— this can take 30-90s on first launch ...",
    file=sys.stderr, flush=True,
)

try:
    from edgar import set_identity
    set_identity(EDGAR_IDENTITY)
except ImportError:
    print(
        "[edgar_mcp_server] FATAL: edgartools not installed. "
        "Run: pip install edgartools",
        file=sys.stderr, flush=True,
    )
    raise SystemExit(1)

# Now load all the Pinpunch EDGAR submodules via our custom importer.
# IMPORTANT: load `base` FIRST — every forms_* module does `from .base
# import ...` at the top, which requires `pinpunch_edgar.base` to already
# be in sys.modules. This also forces edgartools' filings-index hydration
# to happen NOW, before any tool call comes in.
edgar_base   = _load_pinpunch_edgar_module("base",          "base.py")
forms_8k      = _load_pinpunch_edgar_module("forms_8k",      "forms_8k.py")
forms_10k     = _load_pinpunch_edgar_module("forms_10k",     "forms_10k.py")
forms_10q     = _load_pinpunch_edgar_module("forms_10q",     "forms_10q.py")
forms_insider = _load_pinpunch_edgar_module("forms_insider", "forms_insider.py")
forms_13f     = _load_pinpunch_edgar_module("forms_13f",     "forms_13f.py")
financials    = _load_pinpunch_edgar_module("financials",    "financials.py")

print(
    "[edgar_mcp_server] edgartools warm — ready to serve queries",
    file=sys.stderr, flush=True,
)


# ─────────────────────────────────────────────────────────────────────────────
# Server definition
# ─────────────────────────────────────────────────────────────────────────────

def build_mcp_server() -> Any:
    """Build and return the FastMCP server instance with all EDGAR tools."""
    if not MCP_SERVER_AVAILABLE:
        raise RuntimeError(
            "FastMCP not installed. Run: pip install mcp[cli] or pip install fastmcp"
        )

    mcp = FastMCP(
        name="pinpunch-edgar",
        instructions=(
            "SEC EDGAR filings tools — 8-K material events, 10-K annual reports, "
            "10-Q quarterlies, Form 4 insider transactions, 13-F institutional "
            "holdings, and XBRL financial statements. Use these to research "
            "company filings, insider activity, and fundamental data. "
            "All data comes directly from sec.gov via the edgartools library."
        ),
    )

    # ═════════════════════════════════════════════════════════════════════════
    # 8-K Tools (material events: earnings, leadership, M&A, agreements)
    # ═════════════════════════════════════════════════════════════════════════

    @mcp.tool()
    def get_latest_8k(ticker: str) -> dict[str, Any]:
        """
        Fetch the most recent 8-K filing for a ticker.

        Args:
            ticker: Stock ticker (e.g. NVDA, AAPL, TSLA)

        Returns:
            Dict with accession_number, filing_date, items list, and metadata.
            8-K items indicate material event types — e.g. 2.02 = earnings,
            5.02 = leadership change, 1.01 = material agreement, 1.02 = termination,
            2.01 = acquisition completed.
        """
        return forms_8k.get_latest_8k(ticker)

    @mcp.tool()
    def get_8k_events(ticker: str, limit: int = 30) -> dict[str, Any]:
        """
        Fetch recent 8-K filings (raw, uncategorized).

        Args:
            ticker: Stock ticker
            limit:  Max number of filings to return (default 30)

        Returns:
            Dict with a list of 8-K filings, each with date, items, accession.
        """
        return forms_8k.get_8k_events(ticker, limit=limit)

    @mcp.tool()
    def get_8k_events_categorized(ticker: str, limit: int = 30) -> dict[str, Any]:
        """
        Fetch recent 8-K filings grouped by event category.

        Args:
            ticker: Stock ticker
            limit:  Max number of filings to scan (default 30)

        Returns:
            Dict with filings sorted into buckets: earnings, management_changes,
            mergers_acquisitions, agreements, other. Each bucket is a list of
            filings (date, items, accession).
        """
        return forms_8k.get_8k_events_categorized(ticker, limit=limit)

    @mcp.tool()
    def get_8k_full_text(ticker: str, max_length: Optional[int] = None) -> dict[str, Any]:
        """
        Fetch the full text body of the most recent 8-K filing.

        Args:
            ticker:     Stock ticker
            max_length: Optional cap on returned text length (chars)

        Returns:
            Dict with full text body, accession_number, filing_date.
            Use for deep reading of specific high-leverage filings.
        """
        return forms_8k.get_8k_full_text(ticker, max_length=max_length)

    @mcp.tool()
    def search_8k(ticker: str, query: str, max_results: int = 10) -> dict[str, Any]:
        """
        Search recent 8-K filings for a keyword/phrase.

        Args:
            ticker:      Stock ticker
            query:       Search term (e.g. "acquisition", "CEO", "guidance")
            max_results: Max matches to return

        Returns:
            Dict with matching filings + snippets where the query appeared.
        """
        return forms_8k.search_8k(ticker, query, max_results=max_results)

    # ═════════════════════════════════════════════════════════════════════════
    # 10-K Tools (annual reports)
    # ═════════════════════════════════════════════════════════════════════════

    @mcp.tool()
    def get_latest_10k(ticker: str) -> dict[str, Any]:
        """Fetch metadata for the most recent 10-K annual report."""
        return forms_10k.get_latest_10k(ticker)

    @mcp.tool()
    def extract_10k_sections(
        ticker: str,
        sections: Optional[List[str]] = None,
    ) -> dict[str, Any]:
        """
        Extract specific sections from the latest 10-K.

        Args:
            ticker:   Stock ticker
            sections: List of section names to extract. Common sections:
                      business, risk_factors, mda (Management Discussion &
                      Analysis), properties, legal_proceedings.
                      If None, returns all sections.

        Returns:
            Dict mapping section name → extracted text.
        """
        return forms_10k.extract_10k_sections(ticker, sections=sections)

    @mcp.tool()
    def get_10k_full_text(ticker: str, max_length: Optional[int] = None) -> dict[str, Any]:
        """Fetch the full text body of the most recent 10-K."""
        return forms_10k.get_10k_full_text(ticker, max_length=max_length)

    @mcp.tool()
    def get_10k_markdown(ticker: str) -> dict[str, Any]:
        """Fetch the most recent 10-K as Markdown (better structure than plain text)."""
        return forms_10k.get_10k_markdown(ticker)

    @mcp.tool()
    def search_10k(ticker: str, query: str, max_results: int = 10) -> dict[str, Any]:
        """Search the most recent 10-K for a keyword/phrase."""
        return forms_10k.search_10k(ticker, query, max_results=max_results)

    @mcp.tool()
    def get_10k_exhibits(ticker: str) -> dict[str, Any]:
        """List the exhibits attached to the most recent 10-K filing."""
        return forms_10k.get_10k_exhibits(ticker)

    @mcp.tool()
    def get_10k_metadata(ticker: str) -> dict[str, Any]:
        """Fetch metadata (filing date, accession, fiscal year) for the most recent 10-K."""
        return forms_10k.get_10k_metadata(ticker)

    # ═════════════════════════════════════════════════════════════════════════
    # 10-Q Tools (quarterly reports)
    # ═════════════════════════════════════════════════════════════════════════

    @mcp.tool()
    def get_latest_10q(ticker: str) -> dict[str, Any]:
        """Fetch metadata for the most recent 10-Q quarterly report."""
        return forms_10q.get_latest_10q(ticker)

    @mcp.tool()
    def extract_10q_sections(
        ticker: str,
        sections: Optional[List[str]] = None,
    ) -> dict[str, Any]:
        """
        Extract specific sections from the latest 10-Q.

        Args:
            ticker:   Stock ticker
            sections: List of sections to extract (e.g. mda, risk_factors,
                      financial_statements). If None, returns all.

        Returns:
            Dict mapping section name → extracted text.
        """
        return forms_10q.extract_10q_sections(ticker, sections=sections)

    @mcp.tool()
    def get_10q_full_text(ticker: str, max_length: Optional[int] = None) -> dict[str, Any]:
        """Fetch the full text body of the most recent 10-Q."""
        return forms_10q.get_10q_full_text(ticker, max_length=max_length)

    @mcp.tool()
    def get_10q_markdown(ticker: str) -> dict[str, Any]:
        """Fetch the most recent 10-Q as Markdown."""
        return forms_10q.get_10q_markdown(ticker)

    @mcp.tool()
    def get_10q_metadata(ticker: str) -> dict[str, Any]:
        """Fetch metadata for the most recent 10-Q (filing date, period, accession)."""
        return forms_10q.get_10q_metadata(ticker)

    @mcp.tool()
    def search_10q(ticker: str, query: str, max_results: int = 10) -> dict[str, Any]:
        """Search the most recent 10-Q for a keyword/phrase."""
        return forms_10q.search_10q(ticker, query, max_results=max_results)

    # ═════════════════════════════════════════════════════════════════════════
    # Insider / Form 4 Tools
    # ═════════════════════════════════════════════════════════════════════════

    @mcp.tool()
    def get_insider_transactions(ticker: str, limit: int = 25) -> dict[str, Any]:
        """
        Fetch recent insider Form 4 transactions (raw).

        Args:
            ticker: Stock ticker
            limit:  Max transactions to return (default 25)

        Returns:
            Dict with transaction list (insider, role, date, type, shares).
        """
        return forms_insider.get_insider_transactions(ticker, limit=limit)

    @mcp.tool()
    def get_insider_transactions_detailed(
        ticker: str,
        limit: int = 25,
    ) -> dict[str, Any]:
        """
        Fetch insider transactions with full details ($ values, post-tx holdings).

        NOTE: Slower than get_insider_transactions because it pulls each Form 4
        body. Use limit cautiously.
        """
        return forms_insider.get_insider_transactions_detailed(ticker, limit=limit)

    @mcp.tool()
    def get_insider_summary(ticker: str, limit: int = 50) -> dict[str, Any]:
        """
        Aggregated insider activity summary (buys vs sells, share volumes).

        Args:
            ticker: Stock ticker
            limit:  Number of recent transactions to summarize over

        Returns:
            Dict with buys count, sells count, total share volumes,
            most active insiders. Faster than detailed — no $ values.
        """
        return forms_insider.get_insider_summary(ticker, limit=limit)

    # ═════════════════════════════════════════════════════════════════════════
    # 13-F Tools (institutional holdings)
    # ═════════════════════════════════════════════════════════════════════════

    @mcp.tool()
    def get_13f_holdings(ticker: str, quarters: int = 2) -> dict[str, Any]:
        """
        Get institutional holdings reported on 13-F for a ticker.

        Args:
            ticker:   Stock ticker (the institutional manager's CIK, NOT the
                      held stock. E.g. BRK-B for Berkshire Hathaway's 13-F.)
            quarters: Number of recent quarters to fetch

        Returns:
            Dict with quarterly holdings lists.
        """
        return forms_13f.get_13f_holdings(ticker, quarters=quarters)

    @mcp.tool()
    def get_13f_top_holdings(ticker: str, top_n: int = 20) -> dict[str, Any]:
        """Get the top N holdings from the most recent 13-F."""
        return forms_13f.get_13f_top_holdings(ticker, top_n=top_n)

    @mcp.tool()
    def get_13f_manager_info(ticker: str) -> dict[str, Any]:
        """Get the institutional manager's info (name, AUM, address)."""
        return forms_13f.get_13f_manager_info(ticker)

    @mcp.tool()
    def get_13f_summary(ticker: str) -> dict[str, Any]:
        """Aggregated 13-F summary: total holdings count, total value, top sectors."""
        return forms_13f.get_13f_summary(ticker)

    # ═════════════════════════════════════════════════════════════════════════
    # Financials (XBRL fundamentals)
    # ═════════════════════════════════════════════════════════════════════════

    @mcp.tool()
    def get_financials(
        ticker: str,
        periods: int = 4,
        annual: bool = True,
    ) -> dict[str, Any]:
        """
        Get financial statements (income, balance, cash flow) from XBRL.

        Args:
            ticker:  Stock ticker
            periods: Number of historical periods to fetch
            annual:  True for annual (10-K), False for quarterly (10-Q)

        Returns:
            Dict with income_statement, balance_sheet, cash_flow_statement —
            each containing per-period values for standard line items.
        """
        return financials.get_financials(ticker, periods=periods, annual=annual)

    @mcp.tool()
    def get_financial_metrics(ticker: str) -> dict[str, Any]:
        """
        Get derived financial ratios and metrics.

        Returns: Dict with P/E, P/B, ROE, debt-to-equity, current ratio,
                 gross margin, operating margin, etc. — most recent values.
        """
        return financials.get_financial_metrics(ticker)

    @mcp.tool()
    def get_xbrl_statements(ticker: str, form: str = "10-K") -> dict[str, Any]:
        """
        Fetch raw XBRL financial statements.

        Args:
            ticker: Stock ticker
            form:   Form type — "10-K" (annual) or "10-Q" (quarterly)

        Returns: Dict with raw XBRL line items (more detail than get_financials).
        """
        return financials.get_xbrl_statements(ticker, form=form)

    return mcp


# ─────────────────────────────────────────────────────────────────────────────
# Entry point
# ─────────────────────────────────────────────────────────────────────────────

def main() -> None:
    import argparse

    parser = argparse.ArgumentParser(
        description="Pinpunch EDGAR MCP server (long-lived, stdio or http)",
    )
    parser.add_argument(
        "--transport",
        choices=["stdio", "http"],
        default="stdio",
        help="stdio for Claude Code; http for remote/curl testing",
    )
    parser.add_argument("--port", type=int, default=18766)
    parser.add_argument("--host", default="127.0.0.1")
    args = parser.parse_args()

    server = build_mcp_server()

    if args.transport == "stdio":
        # JSON-RPC over stdin/stdout. NEVER print to stdout — corrupts the stream.
        # All logging goes to stderr.
        print(
            "[edgar_mcp_server] starting (stdio transport)",
            file=sys.stderr, flush=True,
        )
        server.run(transport="stdio")
        return

    # HTTP transport (for testing / non-stdio clients)
    print(
        f"[edgar_mcp_server] starting on http://{args.host}:{args.port}/mcp",
        flush=True,
    )
    try:
        server.settings.host = args.host
        server.settings.port = args.port
        server.settings.streamable_http_path = "/mcp"
        server.run(transport="streamable-http")
    except (AttributeError, TypeError):
        server.run(transport="streamable-http", host=args.host, port=args.port, path="/mcp")


if __name__ == "__main__":
    main()
