"""Safety checks for hook boundaries at Thumb IT instructions."""
import sys
import unittest
from pathlib import Path
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from hook_codegen import hook_prefix


class HookPrefixTest(unittest.TestCase):
    def decode(self, hex_bytes):
        decoder = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
        decoder.detail = True
        return hook_prefix(decoder, bytes.fromhex(hex_bytes), 0xd7b80)

    def test_real_settings_prefix_finishes_it(self):
        instructions, conditional = self.decode('002918bf00282de9f0410cbf01240024')
        self.assertEqual([i.mnemonic for i in instructions], ['cmp', 'it', 'cmpne'])
        self.assertEqual(sum(i.size for i in instructions), 6)
        self.assertEqual(conditional, {0xd7b84})

    def test_multi_instruction_it_finishes(self):
        instructions, conditional = self.decode('00291cbf0028012210b5')
        self.assertEqual(sum(i.size for i in instructions), 8)
        self.assertEqual(len(conditional), 2)

    def test_normal_prefix_unchanged(self):
        instructions, conditional = self.decode('10b504460020')
        self.assertEqual(sum(i.size for i in instructions), 4)
        self.assertFalse(conditional)

    def test_following_it_remains_in_original_code(self):
        instructions, conditional = self.decode('0029002818bf0120')
        self.assertEqual(sum(i.size for i in instructions), 4)
        self.assertFalse(conditional)

    def test_truncated_it_is_rejected(self):
        with self.assertRaises(ValueError):
            self.decode('002918bf')

    def test_pc_literal_inside_it_is_rejected(self):
        with self.assertRaises(ValueError):
            self.decode('002918bf0048')

    def test_branch_inside_it_is_rejected(self):
        with self.assertRaises(ValueError):
            self.decode('002918bf00e0')

    def test_pc_write_inside_it_is_rejected(self):
        with self.assertRaises(ValueError):
            self.decode('002918bf00bd')


if __name__ == '__main__':
    unittest.main()
