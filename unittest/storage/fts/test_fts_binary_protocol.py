# Copyright (c) 2026 OceanBase.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import struct
import unittest
from fts_binary_protocol import decode_binary_row

class BinaryRowTest(unittest.TestCase):
    def test_double_bits_and_signed_id(self):
        row = b'\x00\x00' + struct.pack('<qd', -7, 1.25)
        self.assertEqual((-7, '3ff4000000000000'), decode_binary_row(row, [(8, 0), (5, 0)]))
    def test_signed_zero_is_not_rounded_away(self):
        row = b'\x00\x00' + struct.pack('<qQ', 9, 1 << 63)
        self.assertEqual((9, '8000000000000000'), decode_binary_row(row, [(8, 0), (5, 0)]))
    def test_integer_width_and_null_score(self):
        row = b'\x00\x08' + struct.pack('<i', 3)
        self.assertEqual((3, 'NULL'), decode_binary_row(row, [(3, 0), (5, 0)]))
    def test_truncated_rows_fail(self):
        with self.assertRaises(ValueError):
            decode_binary_row(b'\x00\x00\x01', [(8, 0), (5, 0)])
    def test_unsupported_score_type_fails(self):
        with self.assertRaises(ValueError):
            decode_binary_row(b'\x00\x00' + b'\x00' * 16, [(8, 0), (253, 0)])
if __name__ == '__main__':
    unittest.main()
