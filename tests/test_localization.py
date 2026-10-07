from __future__ import annotations

import ast
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
QML_FILES = sorted((ROOT / "src" / "kcm" / "ui").rglob("*.qml"))
CPP_FILES = (
    sorted((ROOT / "src" / "backend").glob("*.cpp"))
    + sorted((ROOT / "src" / "preview").glob("*.cpp"))
    + sorted((ROOT / "src" / "kcm").glob("*.cpp"))
)
SWEDISH_CATALOG = ROOT / "po" / "sv" / "kcm_kfaceauth.po"


def catalog_entries() -> dict[str, str]:
    entries: dict[str, str] = {}
    msgid = ""
    msgstr = ""
    field = ""

    def flush() -> None:
        nonlocal msgid, msgstr, field
        if msgid:
            entries[msgid] = msgstr
        msgid = ""
        msgstr = ""
        field = ""

    for line in SWEDISH_CATALOG.read_text(encoding="utf-8").splitlines():
        if not line:
            flush()
        elif line.startswith("msgid "):
            field = "msgid"
            msgid = ast.literal_eval(line.removeprefix("msgid "))
        elif line.startswith("msgstr ") or line.startswith("msgstr[0] "):
            field = "msgstr"
            msgstr = ast.literal_eval(line.split(" ", 1)[1])
        elif line.startswith('"'):
            value = ast.literal_eval(line)
            if field == "msgid":
                msgid += value
            elif field == "msgstr":
                msgstr += value
    flush()
    return entries


class LocalizationTests(unittest.TestCase):
    def test_every_single_line_user_message_has_swedish_text(self) -> None:
        entries = catalog_entries()
        missing: list[str] = []
        pattern = re.compile(r'\bi18n\("((?:[^"\\]|\\.)*)"')
        plural_pattern = re.compile(
            r'\bi18np\("((?:[^"\\]|\\.)*)",\s*"((?:[^"\\]|\\.)*)"'
        )

        for path in QML_FILES:
            text = path.read_text(encoding="utf-8")
            for raw in pattern.findall(text):
                msgid = ast.literal_eval(f'"{raw}"')
                if not entries.get(msgid):
                    missing.append(f"{path.relative_to(ROOT)}: {msgid}")
            for singular, _plural in plural_pattern.findall(text):
                msgid = ast.literal_eval(f'"{singular}"')
                if msgid not in entries:
                    missing.append(f"{path.relative_to(ROOT)}: {msgid}")

        backend_pattern = re.compile(
            r'(?<!::)\b(?:translate|userText)\(\s*((?:"(?:[^"\\]|\\.)*"\s*)+)\)',
            re.DOTALL,
        )
        direct_pattern = re.compile(
            r'QCoreApplication::translate\("[^"]+",\s*((?:"(?:[^"\\]|\\.)*"\s*)+)\)',
            re.DOTALL,
        )
        for path in CPP_FILES:
            text = path.read_text(encoding="utf-8")
            if "QCoreApplication::translate(" in text:
                missing.append(
                    f"{path.relative_to(ROOT)}: use the installed KI18n gettext catalog, not Qt-only translations"
                )
            if re.search(r"\b(?:translate|userText)\(const char \*text\)", text) and (
                'i18nd("kcm_kfaceauth", text)' not in text
            ):
                missing.append(
                    f"{path.relative_to(ROOT)}: native status translations must use the kcm_kfaceauth gettext domain"
                )
            for literals in backend_pattern.findall(text):
                msgid = "".join(
                    ast.literal_eval(literal)
                    for literal in re.findall(r'"(?:[^"\\]|\\.)*"', literals)
                )
                if not entries.get(msgid):
                    missing.append(f"{path.relative_to(ROOT)}: {msgid}")
            for literals in direct_pattern.findall(text):
                msgid = "".join(
                    ast.literal_eval(literal)
                    for literal in re.findall(r'"(?:[^"\\]|\\.)*"', literals)
                )
                if not entries.get(msgid):
                    missing.append(f"{path.relative_to(ROOT)}: {msgid}")

        self.assertEqual(missing, [])


if __name__ == "__main__":
    unittest.main()
