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

"""Minimal binary-result adapter over the already-used PyMySQL connection.

Only integer IDs and unconverted 64-bit DOUBLE scores are accepted. Unsupported
metadata and malformed packets fail closed rather than weakening a snapshot.
"""
import struct
from pymysql.constants import COMMAND
from pymysql.protocol import FieldDescriptorPacket

_INTEGER_WIDTH = {1: 1, 2: 2, 3: 4, 8: 8, 9: 4, 13: 2}

def decode_binary_row(data, fields):
    if len(fields) != 2 or len(data) < 2 or data[0] != 0:
        raise ValueError('invalid two-column binary row')
    null_bytes = (len(fields) + 9) // 8
    if len(data) < 1 + null_bytes:
        raise ValueError('truncated null bitmap')
    nulls = int.from_bytes(data[1:1 + null_bytes], 'little')
    offset = 1 + null_bytes
    result = []
    for index, (kind, flags) in enumerate(fields):
        if nulls & (1 << (index + 2)):
            result.append(None if index == 0 else 'NULL')
            continue
        if index == 0:
            if kind not in _INTEGER_WIDTH:
                raise ValueError('snapshot ID must be an integer')
            width = _INTEGER_WIDTH[kind]
        else:
            if kind != 5:
                raise ValueError('snapshot score must be a binary DOUBLE')
            width = 8
        if offset + width > len(data):
            raise ValueError('truncated binary value')
        value = data[offset:offset + width]
        offset += width
        if index == 0:
            result.append(int.from_bytes(value, 'little', signed=not (flags & 32)))
        else:
            result.append(f'{int.from_bytes(value, "little"):016x}')
    if offset != len(data) or result[0] is None:
        raise ValueError('unexpected row suffix or null ID')
    return tuple(result)

def _lenenc(value):
    size = len(value)
    if size < 251:
        return bytes([size]) + value
    if size <= 65535:
        return b'\xfc' + struct.pack('<H', size) + value
    if size <= 0xffffff:
        return b'\xfd' + size.to_bytes(3, 'little') + value
    return b'\xfe' + struct.pack('<Q', size) + value

class BinaryScoreClient:
    def __init__(self, connection):
        self.connection = connection
        self.statement = None
        self.sql = None
        self.parameters = 0

    def close(self):
        if self.statement is not None:
            self.connection._execute_command(COMMAND.COM_STMT_CLOSE, struct.pack('<I', self.statement))
            self.statement = None
            self.sql = None

    def _eof(self):
        packet = self.connection._read_packet()
        if not packet.is_eof_packet():
            raise ValueError('expected metadata EOF (deprecate-EOF is not enabled)')
        self.connection.server_status = struct.unpack_from('<H', packet.get_all_data(), 3)[0]

    def execute(self, sql, argument=None):
        c = self.connection
        if sql != self.sql:
            self.close()
            c._execute_command(COMMAND.COM_STMT_PREPARE, sql.encode(c.encoding))
            packet = c._read_packet().get_all_data()
            if len(packet) < 12 or packet[0] != 0:
                raise ValueError('invalid prepare response')
            statement, columns, parameters = struct.unpack_from('<IHH', packet, 1)
            for _ in range(parameters):
                c._read_packet()
            if parameters:
                self._eof()
            for _ in range(columns):
                c._read_packet()
            if columns:
                self._eof()
            self.statement, self.sql, self.parameters = statement, sql, parameters
        payload = struct.pack('<IBI', self.statement, 0, 1)
        if self.parameters:
            if argument is None:
                raise ValueError('missing prepared argument')
            value = argument.encode(c.encoding)
            payload += bytes((self.parameters + 7) // 8) + b'\x01'
            payload += b'\xfd\x00' * self.parameters
            payload += _lenenc(value) * self.parameters
        c._execute_command(COMMAND.COM_STMT_EXECUTE, payload)
        packet = c._read_packet()
        columns = packet.read_length_encoded_integer()
        if columns != 2:
            raise ValueError('snapshot requires two result columns')
        fields = []
        for _ in range(columns):
            packet = c._read_packet()
            field = FieldDescriptorPacket(packet.get_all_data(), c.encoding)
            fields.append((field.type_code, field.flags))
        self._eof()
        result = []
        while True:
            packet = c._read_packet()
            if packet.is_eof_packet():
                c.server_status = struct.unpack_from('<H', packet.get_all_data(), 3)[0]
                break
            result.append(decode_binary_row(packet.get_all_data(), fields))
        return result
