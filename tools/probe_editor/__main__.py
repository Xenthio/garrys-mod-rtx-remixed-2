"""Desktop entry point; the old HTTP viewer is an explicit diagnostic option."""

from pathlib import Path
import sys

if "--browser" in sys.argv or "--no-browser" in sys.argv:
    if "--browser" in sys.argv:
        sys.argv.remove("--browser")
    from .server import main
else:
    dependencies = Path(__file__).resolve().parents[2] / "_work/deps/probe-editor"
    if dependencies.is_dir():
        sys.path.insert(0, str(dependencies))
    try:
        from .desktop import main
    except ImportError as error:
        raise SystemExit(
            "Desktop dependencies missing. Run tools/launch_cubemap_editor.ps1 "
            "or install tools/probe_editor/requirements-desktop.txt. " + str(error)
        )

raise SystemExit(main())
