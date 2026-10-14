"""Compile UTF-8 menu catalogs to the game's CP1251/CP1252 glyph bytes."""
import argparse
import json
from pathlib import Path


DEFAULT_CATALOG = Path(__file__).resolve().parents[1] / "assets/localization/native_menu_ru.json"
DEFAULT_SUBTITLE_CATALOG = Path(__file__).resolve().parents[1] / "assets/localization/subtitle_menu.json"
SUBTITLE_LANGUAGES = {"en": 1, "de": 3, "fr": 4, "es": 5, "it": 6}
SUBTITLE_STRING_LIMITS = {"SUBTITLE SIZE": 16, "LEFT/RIGHT: CHANGE; CONFIRM: NEXT": 40}


def load_catalog(path=DEFAULT_CATALOG):
    catalog = json.loads(Path(path).read_text(encoding="utf-8"))
    if (catalog.get("format"), catalog.get("language"), catalog.get("encoding")) != (1, "ru", "cp1251"):
        raise ValueError("Unsupported native menu catalog format, language or encoding")
    entries = catalog.get("strings")
    if not isinstance(entries, dict) or not entries:
        raise ValueError("Native menu catalog has no strings")
    result = {}
    for english, entry in entries.items():
        if not english or not english.isascii() or english != english.upper():
            raise ValueError(f"Expected an uppercase English menu key: {english!r}")
        if not isinstance(entry, dict):
            raise ValueError(f"Invalid native menu entry: {english}")
        text, maximum = entry.get("text"), entry.get("max_glyphs")
        if not isinstance(text, str) or not text or text != text.upper():
            raise ValueError(f"Expected uppercase Russian menu text: {english}")
        if type(maximum) is not int or not 1 <= maximum <= 40:
            raise ValueError(f"Invalid native menu glyph limit: {english}")
        if any(ord(c) < 32 or ord(c) == 127 for c in english + text):
            raise ValueError(f"Native menu text contains a control character: {english}")
        try:
            encoded = text.encode("cp1251")
        except UnicodeEncodeError as error:
            raise ValueError(f"Native menu text is not representable in CP1251: {english}") from error
        if len(encoded) > maximum:
            raise ValueError(f"Native menu text exceeds {maximum} glyphs: {english}")
        # Registry preserves arbitrary byte strings via its Latin1 transport.
        # Both the XCR and C++ dictionary must contain these exact glyph bytes.
        result[english] = encoded.decode("latin1")
    return result


def load_subtitle_catalog(path=DEFAULT_SUBTITLE_CATALOG):
    def unique_object(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"Duplicate subtitle menu catalog key: {key}")
            result[key] = value
        return result

    catalog = json.loads(Path(path).read_text(encoding="utf-8"), object_pairs_hook=unique_object)
    if not isinstance(catalog, dict) or type(catalog.get("format")) is not int or \
            (catalog.get("format"), catalog.get("encoding")) != (1, "cp1252"):
        raise ValueError("Unsupported subtitle menu catalog format or encoding")
    languages = catalog.get("languages")
    if not isinstance(languages, dict) or set(languages) != set(SUBTITLE_LANGUAGES):
        raise ValueError("Subtitle menu catalog must contain exactly en/de/fr/es/it")
    result = {}
    for language, entries in languages.items():
        if not isinstance(entries, dict) or set(entries) != set(SUBTITLE_STRING_LIMITS):
            raise ValueError(f"Subtitle menu catalog must contain both required keys: {language}")
        translated = {}
        for english, entry in entries.items():
            if not english.isascii() or english != english.upper():
                raise ValueError(f"Expected an uppercase English subtitle menu key: {english!r}")
            if not isinstance(entry, dict) or set(entry) != {"text", "max_glyphs"}:
                raise ValueError(f"Invalid subtitle menu entry: {language}/{english}")
            text, maximum = entry["text"], entry["max_glyphs"]
            if not isinstance(text, str) or not text or text != text.upper():
                raise ValueError(f"Expected uppercase subtitle menu text: {language}/{english}")
            if type(maximum) is not int or maximum != SUBTITLE_STRING_LIMITS[english]:
                raise ValueError(f"Subtitle menu glyph limit must match its fixed row: {language}/{english}")
            if any(ord(c) < 32 or ord(c) == 127 for c in text):
                raise ValueError(f"Subtitle menu text contains a control character: {language}/{english}")
            try:
                encoded = text.encode("cp1252")
            except UnicodeEncodeError as error:
                raise ValueError(f"Subtitle menu text is not representable in CP1252: {language}/{english}") from error
            if any(0x80 <= byte <= 0x9f for byte in encoded):
                raise ValueError(f"Subtitle menu text uses an unsupported Western font glyph: {language}/{english}")
            if len(encoded) > maximum:
                raise ValueError(f"Subtitle menu text exceeds {maximum} glyphs: {language}/{english}")
            if language == "en" and text != english:
                raise ValueError(f"English subtitle menu text must retain its source key: {english}")
            translated[english] = encoded.decode("latin1")
        result[language] = translated
    return result


def compile_header(translations, subtitleTranslations=None):
    if subtitleTranslations is None:
        subtitleTranslations = load_subtitle_catalog()
    lines = ["// Generated from assets/localization/native_menu_ru.json. Do not edit.",
             "#pragma once", "#include <cstdint>", "#include <string_view>", "",
             "namespace DarkRecomp::Native {",
             "struct NativeMenuTranslation { std::string_view english, russian; };",
             "inline constexpr NativeMenuTranslation kRussianNativeMenuText[]{"]
    for english, russian in sorted(translations.items()):
        encoded = "".join(f"\\x{byte:02x}" for byte in russian.encode("latin1"))
        lines.append(f"    {{{json.dumps(english)}, \"{encoded}\"}},")
    lines.extend(["};", "",
                  "constexpr std::string_view russianNativeMenuText(std::string_view english) noexcept {",
                  "    for (const auto& entry : kRussianNativeMenuText)",
                  "        if (entry.english == english) return entry.russian;",
                  "    return english;", "}", "",
                  "// Subtitle rows are generated from assets/localization/subtitle_menu.json.",
                  "struct NativeSubtitleMenuTranslation { uint32_t language; std::string_view english, text; };",
                  "inline constexpr NativeSubtitleMenuTranslation kSubtitleNativeMenuText[]{"])
    for language, languageId in SUBTITLE_LANGUAGES.items():
        for english, text in sorted(subtitleTranslations[language].items()):
            encoded = "".join(f"\\x{byte:02x}" for byte in text.encode("latin1"))
            lines.append(f"    {{{languageId}, {json.dumps(english)}, \"{encoded}\"}},")
    lines.extend(["};", "",
                  "constexpr std::string_view subtitleNativeMenuText(uint32_t languageId, std::string_view english) noexcept {",
                  "    for (const auto& entry : kSubtitleNativeMenuText)",
                  "        if (entry.language == languageId && entry.english == english) return entry.text;",
                  "    return english;", "}", "}", ""])
    return "\n".join(lines)


def write_if_changed(path, contents):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_bytes() != contents:
        path.write_bytes(contents)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalog", type=Path, default=DEFAULT_CATALOG)
    parser.add_argument("--subtitle-catalog", type=Path, default=DEFAULT_SUBTITLE_CATALOG)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.resolve() in (args.catalog.resolve(), args.subtitle_catalog.resolve()):
        parser.error("Never overwrite the translation catalog")
    write_if_changed(args.output, compile_header(load_catalog(args.catalog),
                                               load_subtitle_catalog(args.subtitle_catalog)).encode("ascii"))
