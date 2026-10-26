import hashlib
import json
import struct
import sys
from pathlib import Path
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_C1_C0_2, UC_ARM_REG_FPEXC
from elftools.elf.elffile import ELFFile

BINARY = Path(sys.argv[1])
firmware = BINARY.read_bytes()
if hashlib.sha256(firmware).hexdigest() != 'bc054a3f3ba02c2d33ac9a515a4a8638964da779223502286d6a64b517bf1426':
    raise SystemExit('Unsupported MPC executable: this test targets the supplied Gen1 3.9.1 image')
key = (0xc9771b45f9913b87).to_bytes(8, 'little')
salt = bytes(v ^ key[i % 8] for i, v in enumerate(firmware[0x4e1cf2c:0x4e1cf2c + 37])).rstrip(b'\0')
assert len(salt) == 36

class Validator:
    def __init__(self, files, check):
        self.files = files
        self.streams = {}
        self.digest = None
        self.read_order = []
        self.alloc_next = 0x10000000
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        with BINARY.open('rb') as f:
            elf = ELFFile(f)
            for segment in elf.iter_segments():
                if segment['p_type'] != 'PT_LOAD':
                    continue
                start = segment['p_vaddr'] & ~4095
                length = ((segment['p_vaddr'] + segment['p_memsz'] + 4095) & ~4095) - start
                self.uc.mem_map(start, length)
                self.uc.mem_write(segment['p_vaddr'], segment.data())
        self.uc.mem_map(0x10000000, 0x200000)
        self.uc.mem_map(0x20000000, 0x200000)
        self.uc.mem_map(0x30000000, 0x1000)
        self.uc.reg_write(UC_ARM_REG_C1_C0_2, 0xf << 20)
        self.uc.reg_write(UC_ARM_REG_FPEXC, 1 << 30)
        self.uc.reg_write(UC_ARM_REG_SP, 0x201ff000)
        self.uc.reg_write(UC_ARM_REG_LR, 0x30000000)
        self.vtable = self.alloc(32)
        self.write_words(self.vtable, [0x30000010, 0x30000010, 0x30000014, 0x30000018, 0x3000001c, 0x30000020, 0x30000024, 0x30000028])
        salt_ptr = self.alloc(len(salt))
        self.uc.mem_write(salt_ptr, salt)
        self.write_words(0x6b35e34, [salt_ptr, len(salt)])
        items = self.alloc(4 * len(files) or 4)
        for i, name in enumerate(files):
            self.write_words(items + i * 4, [self.string_data(name.encode())])
        vector = self.alloc(12)
        self.write_words(vector, [items, items + 4 * len(files), items + 4 * len(files)])
        check_file = self.alloc(4)
        self.write_words(check_file, [self.string_data(b'check')])
        self.files['check'] = check
        self.uc.reg_write(UC_ARM_REG_R0, vector)
        self.uc.reg_write(UC_ARM_REG_R1, check_file)
        addresses = [0x911f58, 0x91cbb0, 0x9c5374, 0x94da68, 0x92164c,
                     0x964354, 0x91d028, 0x920830, 0x9c6214, 0x964144,
                     0x8e1678, 0x94d99c, 0x925c88, 0x92c900, 0x93352c,
                     0x933538, 0x9866f8, 0x1fba028]
        for address in addresses:
            self.uc.hook_add(UC_HOOK_CODE, self.hook, begin=address, end=address)
        self.uc.hook_add(UC_HOOK_CODE, self.hook, begin=0x4a6688, end=0x4aa07f)
        self.uc.hook_add(UC_HOOK_CODE, self.hook, begin=0x30000000, end=0x30000028)

    def alloc(self, size):
        address = self.alloc_next
        self.alloc_next += (max(size, 16) + 15) & ~15
        assert self.alloc_next < 0x10200000
        return address

    def words(self, address, count=1):
        return struct.unpack('<' + 'I' * count, self.uc.mem_read(address, count * 4))

    def write_words(self, address, values):
        self.uc.mem_write(address, struct.pack('<' + 'I' * len(values), *values))

    def string_data(self, data):
        address = self.alloc(len(data) + 1)
        self.uc.mem_write(address, data + b'\0')
        return address

    def string(self, address):
        pointer = self.words(address)[0]
        return bytes(self.uc.mem_read(pointer, 4096)).split(b'\0')[0]

    def return_value(self, value=None):
        if value is not None:
            self.uc.reg_write(UC_ARM_REG_R0, value & 0xffffffff)
        self.uc.reg_write(UC_ARM_REG_PC, self.uc.reg_read(UC_ARM_REG_LR))

    def stream(self, data, address=None, name=None):
        address = address or self.alloc(32)
        self.write_words(address, [self.vtable])
        self.streams[address] = [data, 0, name]
        return address

    def hook(self, uc, address, size, _):
        r0, r1, r2, r3 = [uc.reg_read(r) for r in [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3]]
        if address == 0x30000000:
            uc.emu_stop()
        elif address in [0x4a66d8, 0x4a6900]:
            self.return_value(self.alloc(r0))
        elif address in [0x4a66cc, 0x911f58, 0x91cbb0, 0x9c5374, 0x30000010]:
            self.return_value()
        elif address in [0x4a66e4, 0x4a6b34]:
            if address == 0x4a6b34:
                assert r2 <= r3
            uc.mem_write(r0, bytes(uc.mem_read(r1, r2)))
            self.return_value(r0)
        elif address == 0x94da68:
            self.return_value(int(self.string(r0).decode() in self.files and self.files[self.string(r0).decode()] is not None))
        elif address == 0x92164c:
            self.write_words(r0, [self.words(r1)[0]])
            self.return_value(r0)
        elif address == 0x964354:
            name = self.string(r0).decode()
            data = self.files.get(name)
            self.return_value(0 if data is None else self.stream(data, name=name))
        elif address == 0x91d028:
            data = bytes(uc.mem_read(r1, r2))
            pointer = self.alloc(len(data))
            uc.mem_write(pointer, data)
            self.write_words(r0, [pointer, len(data)])
            self.return_value(r0)
        elif address == 0x920830:
            pointer, length = self.words(r1, 2)
            self.stream(bytes(uc.mem_read(pointer, length)), address=r0, name='salt')
            self.return_value(r0)
        elif address == 0x9c6214:
            self.digest = bytes(uc.mem_read(r1, 32)).hex()
            self.write_words(r0, [self.string_data(self.digest.encode())])
            self.return_value(r0)
        elif address == 0x964144:
            name = self.string(r1).decode()
            self.write_words(r0, [self.string_data(self.files[name])])
            self.return_value(r0)
        elif address == 0x8e1678:
            a = bytes(uc.mem_read(r0, 4096)).split(b'\0')[0]
            b = bytes(uc.mem_read(r1, 4096)).split(b'\0')[0]
            self.return_value(0 if a == b else -1 if a < b else 1)
        elif 0x30000014 <= address <= 0x30000028:
            data, pos, name = self.streams[r0]
            if address == 0x30000018:
                self.return_value(int(pos >= len(data)))
            elif address == 0x3000001c:
                chunk = data[pos:pos + r2]
                uc.mem_write(r1, chunk)
                self.streams[r0][1] += len(chunk)
                if chunk and name not in self.read_order:
                    self.read_order.append(name)
                self.return_value(len(chunk))
            else:
                raise RuntimeError(f'Unexpected stream method {address:#x}')
        elif 0x4a6688 <= address < 0x4aa080:
            raise RuntimeError(f'Unmodelled external dependency {address:#x}')

    def run(self):
        self.uc.emu_start(0x1fb92a0, 0x30000000, count=2000000)
        assert self.uc.reg_read(UC_ARM_REG_PC) == 0x30000000, 'Instruction limit reached'
        return bool(self.uc.reg_read(UC_ARM_REG_R0))

class Reader(Validator):
    def __init__(self, files):
        super().__init__(dict(files), None)
        self.output = self.alloc(12)
        self.directory = self.alloc(4)
        self.prefix = self.alloc(4)
        self.write_words(self.directory, [self.string_data(b'/usb/autorun')])
        self.write_words(self.prefix, [self.string_data(b'autorun')])
        self.uc.reg_write(UC_ARM_REG_R0, self.output)
        self.uc.reg_write(UC_ARM_REG_R1, self.directory)
        self.uc.reg_write(UC_ARM_REG_R2, self.prefix)

    def cstring(self, pointer):
        return bytes(self.uc.mem_read(pointer, 4096)).split(b'\0')[0]

    def stdstring(self, address, data):
        self.write_words(address, [self.string_data(data), len(data), len(data)])

    def hook(self, uc, address, size, data):
        r0, r1, r2 = [uc.reg_read(r) for r in [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2]]
        if address == 0x94d99c:
            self.return_value(1)
        elif address == 0x925c88:
            self.write_words(r0, [self.words(r1)[0]])
            self.return_value(r0)
        elif address == 0x92c900:
            self.write_words(r0, [self.string_data(self.string(r1) + self.cstring(r2))])
            self.return_value(r0)
        elif address in [0x93352c, 0x933538]:
            self.stdstring(r0, self.string(r1))
            self.return_value(r0)
        elif address == 0x9866f8:
            child = self.string(r1) + b'/' + self.cstring(r2)
            self.write_words(r0, [self.string_data(child)])
            self.return_value(r0)
        elif address == 0x4a6834:
            self.return_value(len(self.cstring(r0)))
        elif address == 0x1fba028:
            value = json.loads(self.files[self.string(r1).decode()])['value0']
            scripts = value['scripts']
            items = self.alloc(100 * len(scripts) or 16)
            for index, script in enumerate(scripts):
                entry = items + index * 100
                self.stdstring(entry, script['path'].encode())
                uc.mem_write(entry + 24, bytes([int(script['askQuestion'])]))
                for offset, key in [(28, 'message'), (52, 'yesButtonText'), (76, 'noButtonText')]:
                    self.stdstring(entry + offset, script[key].encode())
            self.write_words(r0, [items, items + len(scripts) * 100, items + len(scripts) * 100])
            items = self.alloc(24 * len(value['files']) or 16)
            for index, name in enumerate(value['files']):
                self.stdstring(items + index * 24, name.encode())
            self.write_words(r0 + 12, [items, items + len(value['files']) * 24, items + len(value['files']) * 24])
            uc.mem_write(r0 + 24, b'\1')
            self.return_value(r0)
        else:
            super().hook(uc, address, size, data)

    def run(self, instruction_limit=3000000):
        self.uc.emu_start(0x1fbcd28, 0x30000000, count=instruction_limit)
        assert self.uc.reg_read(UC_ARM_REG_PC) == 0x30000000, 'Instruction limit reached'
        begin, end, _ = self.words(self.output, 3)
        assert (end - begin) % 100 == 0
        return (end - begin) // 100

files = {'autorun.json': b'{"value0":{"files":[],"scripts":[]}}', 'probe.sh': b'#!/bin/sh\nprintf "USB launch probe\\n"\n'}
expected = hashlib.sha256(b''.join(files.values()) + salt).hexdigest().encode()
cases = [
    ('valid', files, expected, True),
    ('plain_sha256_without_salt', files, hashlib.sha256(b''.join(files.values())).hexdigest().encode(), False),
    ('modified_script', dict(files, **{'probe.sh': files['probe.sh'] + b'\n'}), expected, False),
    ('modified_json', dict(files, **{'autorun.json': files['autorun.json'] + b'\n'}), expected, False),
    ('wrong_order', dict(reversed(list(files.items()))), expected, False),
    ('missing_check', files, None, False),
    ('missing_script', dict(files, **{'probe.sh': None}), expected, False),
    ('empty_files', {}, hashlib.sha256(salt).hexdigest().encode(), True),
    ('uppercase_check', files, expected.upper(), False),
    ('check_trailing_newline', files, expected + b'\n', False),
]
results = []
for name, inputs, check, should_pass in cases:
    validator = Validator(dict(inputs), check)
    try:
        actual = validator.run()
    except Exception:
        print('Emulation stopped at', hex(validator.uc.reg_read(UC_ARM_REG_PC)), file=sys.stderr)
        raise
    assert actual == should_pass, (name, actual, should_pass, validator.digest)
    results.append({'case': name, 'accepted': actual, 'expected': should_pass, 'passed': True, 'read_order': validator.read_order})
validator_report = {'validator': 'stock MPC 3.9.1 function 0x1fb92a0', 'scope': 'real ARM validator, concatenation and SHA256; modelled filesystem, allocation and string IO dependencies; not full MPC execution', 'tests': results}

value = {'value0': {'files': ['payload.dat'], 'scripts': [{'path': 'probe.sh', 'askQuestion': True, 'message': 'USB launch test', 'yesButtonText': 'Run', 'noButtonText': 'Cancel'}]}}
reader_files = {'/usb/autorun/autorun.json': json.dumps(value).encode(), '/usb/autorun/probe.sh': b'#!/bin/sh\ntrue\n', '/usb/autorun/payload.dat': b'test payload'}
reader_check = hashlib.sha256(b''.join(reader_files.values()) + salt).hexdigest().encode()
reader_files['/usb/autorun/autorun.check'] = reader_check
reader = Reader(reader_files)
try:
    count = reader.run()
except Exception:
    print('Reader emulation stopped at', hex(reader.uc.reg_read(UC_ARM_REG_PC)), file=sys.stderr)
    raise
assert count == 1, (count, reader.digest)
reader_report = {'reader': 'stock MPC 3.9.1 function 0x1fbcd28', 'scope': 'real firmware file ordering, validation, script-copy result; JSON parser and filesystem IO modelled', 'script_count': count, 'read_order': reader.read_order}

reader_cases = []
for name, changed in [('modified_script', {'/usb/autorun/probe.sh': b'changed'}), ('missing_payload', {'/usb/autorun/payload.dat': None}), ('missing_check', {'/usb/autorun/autorun.check': None})]:
    invalid = dict(reader_files, **changed)
    invalid = {key: value for key, value in invalid.items() if value is not None}
    candidate = Reader(invalid)
    actual = candidate.run()
    assert actual == 0, (name, actual)
    reader_cases.append({'case': name, 'script_count': actual, 'passed': True})
reader_report['rejection_tests'] = reader_cases
dispatch_cases = []
for startup, ask, destination in [(1, 1, 0x1fbdb28), (1, 0, 0x1fbdc54),
                                 (0, 1, 0x1fbdec4), (0, 0, 0x1fbdc54)]:
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    uc.mem_map(0x1fbd000, 0x2000)
    uc.mem_write(0x1fbdb10, firmware[0x1fbdb10:0x1fbdb28])
    uc.mem_map(0x10000000, 0x1000)
    uc.reg_write(UC_ARM_REG_SP, 0x10000000)
    uc.mem_write(0x10000090, bytes([ask]))
    uc.mem_write(0x10000024, struct.pack('<I', startup))
    uc.emu_start(0x1fbdb10, destination, count=10)
    assert uc.reg_read(UC_ARM_REG_PC) == destination
    dispatch_cases.append({'startup': bool(startup), 'ask_question': bool(ask),
                           'result': 'execute' if destination == 0x1fbdc54 else
                           'skip' if destination == 0x1fbdb28 else 'prompt', 'passed': True})
reader_report['startup_dispatch_tests'] = dispatch_cases
print(json.dumps({'validator': validator_report, 'reader': reader_report}, indent=2))
