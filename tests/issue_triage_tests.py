#!/usr/bin/env python3
"""Unit tests for the GitHub issue AI triage helper."""

from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / ".github" / "scripts" / "issue_triage.py"
SPEC = importlib.util.spec_from_file_location("issue_triage", MODULE_PATH)
assert SPEC and SPEC.loader
issue_triage = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(issue_triage)


class IssueTriageTests(unittest.TestCase):
    def test_labels_are_whitelisted_deduplicated_and_capped(self) -> None:
        labels = issue_triage.normalize_labels(
            ["bug", "bug", "wontfix", "question", "enhancement"]
        )
        self.assertEqual(labels, ["bug", "question"])

    def test_reply_sanitizes_mentions_and_reserved_marker(self) -> None:
        reply = issue_triage.sanitize_reply(
            "Thanks @octocat. mail@example.com " + issue_triage.MARKER
        )
        self.assertIn("@\u200boctocat", reply)
        self.assertIn("mail@example.com", reply)
        self.assertNotIn(issue_triage.MARKER, reply)

    def test_extract_output_text_ignores_non_message_items(self) -> None:
        response = {
            "output": [
                {"type": "reasoning", "summary": []},
                {
                    "type": "message",
                    "content": [
                        {"type": "output_text", "text": '{"reply":"ok","labels":[]}'},
                    ],
                },
            ]
        }
        self.assertEqual(
            issue_triage.extract_output_text(response),
            '{"reply":"ok","labels":[]}',
        )

    def test_missing_key_uses_japanese_fallback_without_labels(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            event = root / "event.json"
            event.write_text(
                json.dumps({"issue": {"title": "質問です", "body": "動きません"}}),
                encoding="utf-8",
            )
            result = issue_triage.generate(root, event, "", issue_triage.DEFAULT_MODEL)
        self.assertEqual(result["mode"], "fallback-no-key")
        self.assertEqual(result["labels"], [])
        self.assertIn("Issueありがとうございます", result["reply"])

    def test_payload_uses_structured_output_and_does_not_store(self) -> None:
        payload = issue_triage.build_payload("model", "title", "body", "context")
        self.assertFalse(payload["store"])
        self.assertEqual(payload["text"]["format"]["type"], "json_schema")
        self.assertTrue(payload["text"]["format"]["strict"])
        self.assertIn("UNTRUSTED ISSUE JSON", payload["input"])


if __name__ == "__main__":
    unittest.main()
