#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Pulse project; see LICENSE and NOTICE for licensing and attribution.
# Modified by EternalLightning for this independent distribution, including licensing notices.
# Existing third-party notices and license terms remain applicable.

import unittest

from generate_runtime_strings import cpp_literal, generate, parse_rc


def resource(chinese, english=None):
    if english is None:
        english = chinese
    return ("LANGUAGE 0x04, 0x02\nSTRINGTABLE\nBEGIN\n" + chinese +
            "\nEND\nLANGUAGE 0x09, 0x01\nSTRINGTABLE\nBEGIN\n" + english + "\nEND\n")


class RuntimeStringsTest(unittest.TestCase):
    def test_quotes_backslashes_and_controls(self):
        source = resource(r'IDS_TEXT "中文 ""quoted"" C:\\folder\n\r\t\a\b\f\v"')
        value = parse_rc(source)["zh"]["IDS_TEXT"]
        self.assertEqual(value, '中文 "quoted" C:\\folder\n\r\t\a\b\f\v')
        self.assertEqual(cpp_literal(value), r'L"中文 \"quoted\" C:\\folder\n\r\t\a\b\f\v"')

    def test_language_order_and_id_order_are_independent(self):
        source = resource('IDS_A "甲"\nIDS_B "乙"', 'IDS_B "B"\nIDS_A "A"')
        generated = generate(source)
        self.assertIn('static const std::wstring chinese = L"甲";', generated)
        self.assertIn('static const std::wstring english = L"A";', generated)
        self.assertIn("return language == Language::ZhCN ? chinese : english;", generated)
        self.assertEqual(generate(source), generated)
        blocks = source.split("LANGUAGE 0x09, 0x01")
        reversed_source = "LANGUAGE 0x09, 0x01" + blocks[1] + blocks[0]
        self.assertEqual(parse_rc(source), parse_rc(reversed_source))

    def test_invalid_resources_are_rejected(self):
        valid = resource('IDS_A "A"')
        invalid = [
            resource('IDS_A "A"\nIDS_A "again"'),
            resource('IDS_A "A"', 'IDS_B "B"'),
            resource(''),
            resource(r'IDS_A "bad\q"'),
            resource('IDS_A "bad"quote"'),
            resource('IDS_A "A" trailing'),
            resource('#define IDS_A "A"'),
            valid.replace("LANGUAGE 0x09, 0x01", "LANGUAGE 0x04, 0x02"),
            valid.replace("LANGUAGE 0x09, 0x01", "LANGUAGE 0x09, 0x02"),
            valid.removesuffix("END\n"),
            valid + 'IDS_OUTSIDE "A"\n',
        ]
        for source in invalid:
            with self.subTest(source=source):
                with self.assertRaises(ValueError):
                    parse_rc(source)


if __name__ == "__main__":
    unittest.main()
