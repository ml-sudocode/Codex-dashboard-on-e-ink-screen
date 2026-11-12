"""Start the Codex Board collector from the repository root."""
import runpy
import sys
from pathlib import Path


tool = Path(__file__).resolve().parent / "tools" / "codex_board" / "collector.py"
sys.path.insert(0, str(tool.parent))
runpy.run_path(str(tool), run_name="__main__")
