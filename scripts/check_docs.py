#!/usr/bin/env python3
"""Validate repository Markdown links without third-party packages."""

from __future__ import annotations

import os
import re
import subprocess
import sys
from pathlib import Path
from urllib.parse import unquote, urlsplit


INLINE_LINK = re.compile(
    r"!?\[[^\]\n]*\]\(\s*(?:<(?P<angle>[^>\n]+)>|(?P<plain>[^\s)\n]+))"
)
REFERENCE_LINK = re.compile(
    r"^[ \t]{0,3}\[[^\]\n]+\]:[ \t]*(?:<(?P<angle>[^>\n]+)>|(?P<plain>[^\s\n]+))",
    re.MULTILINE,
)
FENCE = re.compile(r"^[ \t]{0,3}(?P<marker>`{3,}|~{3,})")
INLINE_CODE = re.compile(r"(`+)([^\n]*?)\1")


def mask_text(value: str) -> str:
    """Replace non-newline characters so match offsets and line numbers survive."""
    return "".join("\n" if char == "\n" else " " for char in value)


def mask_code(text: str) -> str:
    """Ignore fenced and inline code, where Markdown-looking text is literal."""
    output: list[str] = []
    fence_character = ""
    fence_length = 0

    for line in text.splitlines(keepends=True):
        match = FENCE.match(line)
        if fence_character:
            output.append(mask_text(line))
            if match:
                marker = match.group("marker")
                if marker[0] == fence_character and len(marker) >= fence_length:
                    fence_character = ""
                    fence_length = 0
            continue
        if match:
            marker = match.group("marker")
            fence_character = marker[0]
            fence_length = len(marker)
            output.append(mask_text(line))
            continue
        output.append(line)

    without_fences = "".join(output)
    return INLINE_CODE.sub(lambda match: mask_text(match.group(0)), without_fences)


def repository_paths(root: Path) -> list[str]:
    result = subprocess.run(
        [
            "git",
            "-C",
            str(root),
            "ls-files",
            "--cached",
            "--others",
            "--exclude-standard",
            "-z",
        ],
        check=True,
        stdout=subprocess.PIPE,
    )
    return [os.fsdecode(item) for item in result.stdout.split(b"\0") if item]


def destinations(text: str) -> list[tuple[int, str]]:
    masked = mask_code(text)
    found: list[tuple[int, str]] = []
    for pattern in (INLINE_LINK, REFERENCE_LINK):
        for match in pattern.finditer(masked):
            target = match.group("angle") or match.group("plain")
            line = text.count("\n", 0, match.start()) + 1
            found.append((line, target))
    return found


def relative_destination(destination: str) -> str | None:
    if destination.startswith(("#", "/")):
        return None
    parsed = urlsplit(destination)
    if parsed.scheme or parsed.netloc or not parsed.path:
        return None
    return unquote(parsed.path)


def has_exact_case(root: Path, repository_path: str) -> bool:
    """Check path spelling even on case-insensitive developer filesystems."""
    current = root
    for component in Path(repository_path).parts:
        try:
            names = {entry.name for entry in current.iterdir()}
        except OSError:
            return False
        if component not in names:
            return False
        current /= component
    return current.exists()


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    try:
        repository_files = repository_paths(root)
    except (OSError, subprocess.CalledProcessError) as error:
        print(f"docs: cannot list tracked files: {error}", file=sys.stderr)
        return 2

    markdown = sorted(path for path in repository_files if path.lower().endswith(".md"))
    failures: list[str] = []
    checked = 0

    for source_name in markdown:
        source = root / source_name
        # A tracked file deleted by the current cleanup is not part of the
        # resulting documentation set. Unignored new Markdown is included so
        # pre-commit validation covers additions such as a replacement roadmap.
        if not source.is_file():
            continue
        try:
            text = source.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as error:
            failures.append(f"{source_name}: cannot read UTF-8 Markdown: {error}")
            continue

        for line, destination in destinations(text):
            relative = relative_destination(destination)
            if relative is None:
                continue
            checked += 1
            candidate = (source.parent / relative).resolve()
            try:
                repository_path = candidate.relative_to(root).as_posix()
            except ValueError:
                failures.append(
                    f"{source_name}:{line}: {destination!r} escapes the repository"
                )
                continue

            if not candidate.exists():
                failures.append(
                    f"{source_name}:{line}: {destination!r} has no target"
                )
            elif not has_exact_case(root, repository_path):
                failures.append(
                    f"{source_name}:{line}: {destination!r} has mismatched path casing"
                )

    if failures:
        for failure in failures:
            print(f"docs: {failure}", file=sys.stderr)
        print(
            f"docs: FAILED ({len(failures)} problem(s), {checked} relative link(s) checked)",
            file=sys.stderr,
        )
        return 1

    existing_markdown = sum((root / path).is_file() for path in markdown)
    print(
        f"docs: OK ({checked} relative link(s) in "
        f"{existing_markdown} tracked/new Markdown file(s))"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
