"""Writes Langs/*.ini from langs_data.py.

usage: python tools/gen_langs.py [AIMP Langs folder]
With the AIMP folder given, each file's AIMP= line also lists AIMP's own language name
(the Name= line of its .lng file), which is what "Automatic" compares against.
"""
import os, sys, re
sys.path.insert(0, os.path.dirname(__file__))
from langs_data import FIELDS, LANGS

NATIVE = {
 "english": "English", "russian": "Русский", "spanish": "Español", "ukrainian": "Українська",
 "belarusian": "Беларуская", "kazakh": "Қазақша", "tatar": "Татарча", "bulgarian": "Български",
 "serbian_cyrillic": "Српски (ћирилица)", "serbian_latin": "Srpski (latinica)", "croatian": "Hrvatski",
 "slovenian": "Slovenščina", "czech": "Čeština", "slovak": "Slovenčina", "polish": "Polski",
 "german": "Deutsch", "dutch": "Nederlands", "swedish": "Svenska", "finnish": "Suomi", "eesti": "Eesti",
 "latvian": "Latviešu", "lithuanian": "Lietuvių", "hungarian": "Magyar", "romanian": "Română",
 "french": "Français", "italian": "Italiano", "portuguese": "Português (Portugal)",
 "brazilian": "Português (Brasil)", "greek": "Ελληνικά", "turkish": "Türkçe", "azeri": "Azərbaycanca",
 "turkmen": "Türkmençe", "uzbek": "Oʻzbekcha", "armenian": "Հայերեն", "arabic": "العربية",
 "hebrew": "עברית", "farsi": "فارسی", "indonesian": "Bahasa Indonesia", "vietnamese": "Tiếng Việt",
 "thai": "ไทย", "japanese": "日本語", "korean": "한국어", "simplified_chinese": "简体中文",
 "traditional_chinese": "繁體中文",
}

HEADER = """; Language file for the MPEG-H 3D Audio decoder plugin for AIMP.
;
; To add or fix a language: translate the text after each "=" and save the file as UTF-8.
; Keep the keys on the left unchanged. A new language is a copy of english.ini with a new name.
;   Name - the language's own name, shown in the plugin's Language list
;   AIMP - AIMP language names this file is used for when Language is "Automatic"
;          (comma-separated, any case)
; %ls is replaced by a number; \\n starts a new line. Missing lines are shown in English.
"""

def aimp_name(folder, base):
    if not folder:
        return None
    p = os.path.join(folder, base + ".lng")
    if not os.path.exists(p):
        return None
    with open(p, "rb") as f:
        raw = f.read(8192)
    if raw[:2] in (b"\xff\xfe", b"\xfe\xff"):
        text = raw[: len(raw) // 2 * 2].decode("utf-16", errors="ignore")
    else:
        text = raw.decode("utf-8-sig", errors="ignore")
    m = re.search(r"^\s*Name\s*=\s*(.+?)\s*$", text, re.M)
    return m.group(1).strip().lower() if m else None

def main():
    aimp_dir = sys.argv[1] if len(sys.argv) > 1 else None
    out_dir = os.path.join(os.path.dirname(__file__), "..", "Langs")
    os.makedirs(out_dir, exist_ok=True)
    for lid, (lngs, t) in LANGS.items():
        assert len(t) == len(FIELDS), (lid, len(t))
        f = dict(zip(FIELDS, t))
        names = []
        for base in lngs:
            for n in (base.lower(), aimp_name(aimp_dir, base)):
                if n and n not in names:
                    names.append(n)
        if lid == "spanish":
            names += ["español", "spanish"]
        lines = [HEADER, "[Info]", "Name=" + NATIVE[lid], "AIMP=" + ",".join(names), "", "[Strings]"]
        s = {
            "Title": f["Title"], "Label.Layout": f["LLayout"], "Label.Room": f["LRoom"],
            "Label.Loudness": f["LLoud"], "Label.Drc": f["LDrc"], "Label.Language": f["LLang"],
            "ReadTags": f["ReadTags"], "Note": f["Note"], "OK": f["OK"], "Cancel": f["Cancel"],
            "SaveFailed": f["SaveFailed"], "Custom": f"{f['Custom']} (%ls)",
            "CustomLayout": f"{f['Custom']} (CICP %ls)", "CustomLkfs": f"{f['Custom']} (%ls LKFS)",
            "Language.Auto": f["Auto"],
            "Layout.Headphones": f"{f['Headphones']} (3D)", "Layout.Stereo": f["Stereo"],
            "Layout.5.1": f"5.1 {f['Surround']}", "Layout.7.1": f"7.1 {f['Surround']}",
            "Layout.9.1.6": f"9.1.6 (16 {f['Channels']})", "Layout.Mono": f["Mono"],
            "Room.Off": f"{f['Off']} ({f['Dry']})", "Room.Light": f["Light"], "Room.Default": f["Default"],
            "Room.Medium": f["Medium"], "Room.Large": f["Large"], "Room.Maximum": f["Maximum"],
            "Loudness.Default": f"{f['Default']} (-24 LKFS)", "Loudness.-16": f"-16 LKFS ({f['Louder']})",
            "Loudness.-18": "-18 LKFS", "Loudness.-20": "-20 LKFS", "Loudness.-23": "-23 LKFS",
            "Loudness.-27": "-27 LKFS", "Loudness.-31": f"-31 LKFS ({f['QuietFull']})",
            "Drc.Default": f"{f['Default']} ({f['OnlyClip']})", "Drc.Off": f["Off"],
            "Drc.LateNight": f["LateNight"], "Drc.Noisy": f["Noisy"], "Drc.Limited": f["Limited"],
            "Drc.LowLevel": f["LowLevel"], "Drc.Dialog": f["Dialog"], "Drc.General": f["General"],
        }
        lines += [f"{k}={v}" for k, v in s.items()]
        with open(os.path.join(out_dir, lid + ".ini"), "w", encoding="utf-8-sig", newline="\r\n") as fp:
            fp.write("\n".join(lines) + "\n")
    print(len(LANGS), "language files written")

if __name__ == "__main__":
    main()
