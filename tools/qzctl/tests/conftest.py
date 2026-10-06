import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[2]  # <repo>/tools, so `import qzctl` works from anywhere
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))
