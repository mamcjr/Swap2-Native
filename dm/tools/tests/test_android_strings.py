"""The launcher's string resources in every language (android/app/src/main/res/values*/strings*.xml): each language has every translatable text of the
English one (values/) and no other, with the same placeholders (%s, %1$d, %%, ...) and plural forms, and no text aapt refuses (an unescaped apostrophe or
double quote, a leading @ or ?). The languages are the Switch 2's: LANGUAGES below, one resource folder each. Run: python3 dm/tools/tests/test_android_strings.py"""
import re
import sys
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

RES = Path(__file__).resolve().parents[3] / 'android/app/src/main/res'
# The Switch 2's system languages and the resource folder of each (English is values/; pt is Brazilian Portuguese, pt-rPT Portugal's)
LANGUAGES = {
    'en-US': 'values', 'en-GB': 'values-en-rGB', 'ja': 'values-ja', 'fr': 'values-fr', 'fr-CA': 'values-fr-rCA', 'de': 'values-de',
    'it': 'values-it', 'es': 'values-es', 'es-419': 'values-b+es+419', 'nl': 'values-nl', 'pt-BR': 'values-pt', 'pt-PT': 'values-pt-rPT',
    'ru': 'values-ru', 'ko': 'values-ko', 'zh-Hans': 'values-b+zh+Hans', 'zh-Hant': 'values-b+zh+Hant',
}
PLACEHOLDER = re.compile(r'%(?:\d+\$)?[-#+ 0,(]*\d*(?:\.\d+)?[sdfxXc%]')


def entries(folder):
    """{(tag, name): element} of every strings*.xml of a resource folder"""
    out = {}
    for p in sorted((RES / folder).glob('strings*.xml')):
        for e in ET.parse(p).getroot():
            if e.tag in ('string', 'plurals', 'string-array'):
                out[(e.tag, e.get('name'))] = e
    return out


def texts(e):
    """The element's texts: one for a string, one per quantity or item"""
    if e.tag == 'string':
        return {'': ''.join(e.itertext())}
    if e.tag == 'plurals':
        return {i.get('quantity'): ''.join(i.itertext()) for i in e}
    return {str(n): ''.join(i.itertext()) for n, i in enumerate(e)}


def placeholders(s):
    return sorted(PLACEHOLDER.findall(s))


def bad_escape(s):
    """Why aapt refuses this text, or None"""
    if s.startswith('"') and s.endswith('"') and len(s) > 1:
        return None   # a quoted text keeps apostrophes as they are
    if re.search(r"(?<!\\)'", s):
        return 'unescaped apostrophe (write \\\')'
    if re.search(r'(?<!\\)"', s):
        return 'unescaped double quote (write \\")'
    if s[:1] in ('@', '?'):
        return 'leading %s (write \\%s)' % (s[0], s[0])
    return None


class Strings(unittest.TestCase):
    def test_every_language_matches_english(self):
        base = {k: e for k, e in entries('values').items() if e.get('translatable') != 'false'}
        self.assertTrue(base, 'no English strings in values/')
        problems = []
        for lang, folder in LANGUAGES.items():
            if folder == 'values':
                continue
            have = entries(folder)
            if not have:
                problems.append(f'{lang}: no {folder}/strings*.xml')
                continue
            missing = sorted(n for (t, n) in base if (t, n) not in have)
            extra = sorted(n for (t, n) in have if (t, n) not in base)
            if missing:
                problems.append(f'{lang}: {len(missing)} missing, first {missing[:5]}')
            if extra:
                problems.append(f'{lang}: {len(extra)} not in English, first {extra[:5]}')
            for k, e in base.items():
                if k not in have:
                    continue
                en, tr = texts(e), texts(have[k])
                if k[0] == 'plurals':
                    if 'other' not in tr:
                        problems.append(f'{lang} {k[1]}: plurals without "other"')
                    want = placeholders(en.get('other', ''))
                    for q, s in tr.items():
                        if placeholders(s) != want and not (q == 'one' and placeholders(s) == []):
                            problems.append(f'{lang} {k[1]} ({q}): placeholders {placeholders(s)} != {want}')
                    continue
                if k[0] == 'string-array' and len(tr) != len(en):
                    problems.append(f'{lang} {k[1]}: {len(tr)} items, English has {len(en)}')
                    continue
                for q, s in en.items():
                    if placeholders(tr.get(q, '')) != placeholders(s):
                        problems.append(f'{lang} {k[1]}: placeholders {placeholders(tr.get(q, ""))} != {placeholders(s)}')
        self.assertEqual(problems, [], '\n'.join(problems[:60]))

    def test_no_text_aapt_refuses(self):
        problems = []
        for folder in LANGUAGES.values():
            for k, e in entries(folder).items():
                for q, s in texts(e).items():
                    why = bad_escape(s)
                    if why:
                        problems.append(f'{folder} {k[1]}{" " + q if q else ""}: {why}: {s[:60]!r}')
        self.assertEqual(problems, [], '\n'.join(problems[:60]))

    def test_locale_config_lists_every_language(self):
        cfg = RES / 'xml/locales_config.xml'
        self.assertTrue(cfg.is_file(), 'res/xml/locales_config.xml (the per-app language list, android:localeConfig) is missing')
        ns = '{http://schemas.android.com/apk/res/android}name'
        listed = {e.get(ns) for e in ET.parse(cfg).getroot()}
        self.assertEqual(listed, set(LANGUAGES))


if __name__ == '__main__':
    sys.exit(unittest.main())
