#!/usr/bin/env python3
"""Install Purple Lib source files from a GitHub repository."""
import argparse
import os
import pathlib
import shutil
import subprocess
import tempfile
import sys


def main():
    ap = argparse.ArgumentParser(description="Install Purple Lib files from GitHub")
    ap.add_argument("--githubpage", required=True, metavar="URL", help="GitHub repository URL or owner/repository")
    ap.add_argument("--lib-dir", default=os.path.expanduser("~/.local/share/purplecode/libs"), help="library installation directory")
    ns = ap.parse_args()
    repo = ns.githubpage.strip()
    if not repo.startswith(("https://", "http://", "git@")):
        repo = "https://" + repo
    if "github.com/" in repo and not repo.endswith(".git"):
        repo += ".git"
    dest = pathlib.Path(ns.lib_dir).expanduser()
    dest.mkdir(parents=True, exist_ok=True)
    try:
        with tempfile.TemporaryDirectory(prefix="purplelib-") as td:
            checkout = pathlib.Path(td) / "repo"
            subprocess.run(["git", "clone", "--depth", "1", repo, str(checkout)], check=True)
            files = sorted(p for p in checkout.rglob("*.pl") if p.is_file())
            if not files:
                print("No .pl Purple Lib files found in repository", file=sys.stderr)
                return 1
            for src in files:
                shutil.copy2(src, dest / src.name)
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f"Purple Lib install failed: {exc}", file=sys.stderr)
        return 1
    print(f"Installed {len(files)} Purple Lib file(s) into {dest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
