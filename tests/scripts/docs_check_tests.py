import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("check_docs", Path(__file__).resolve().parents[2] / "scripts/check-docs.py")
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


class DocumentationContracts(unittest.TestCase):
    def test_links_anchors_duplicates_and_examples(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "docs").mkdir()
            (root / "docs/guide.md").write_text("# Guide\n## Same\n## Same\n", encoding="utf-8")
            readme = root / "README.md"
            readme.write_text("[valid](docs/guide.md#same-1)\n```\n[example](missing.md)\n```\n", encoding="utf-8")
            self.assertEqual(checker.check(root), [])
            readme.write_text("[missing](missing.md)\n[anchor](docs/guide.md#absent)\n[escape](../README.md)\n", encoding="utf-8")
            errors = checker.check(root)
            self.assertEqual(len(errors), 3)
            self.assertIn("missing target", errors[0])
            self.assertIn("missing heading", errors[1])
            self.assertIn("escapes repository", errors[2])


if __name__ == "__main__":
    unittest.main()
