#!/usr/bin/env python3
"""Exercise the real installer offline, using local GitHub snapshot responses."""

import io
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[1]
PIN = "1" * 40
NEXT = "2" * 40
MAIN = "3" * 40


def fake_curl(arguments):
    fixtures = Path(os.environ["TIDE_TEST_FIXTURES"])
    with (fixtures / "requests.jsonl").open("a") as log:
        log.write(json.dumps(arguments) + "\n")
    # Check the transport policy as well as providing offline responses.
    for option in ("--proto", "--proto-redir"):
        if arguments[arguments.index(option) + 1] != "=https":
            return 2
    output = Path(arguments[arguments.index("-o") + 1])
    url = next(arg for arg in arguments if arg.startswith("https://"))
    if url == "https://api.github.com/repos/paoloanzn/tide/commits/main":
        source = fixtures / "main-sha"
    elif url.startswith("https://github.com/paoloanzn/tide/archive/"):
        ref = url.split("/archive/", 1)[1].replace("/", "_")
        source = fixtures / ref
    else:
        return 2
    if not source.exists():
        # A failed download can still leave bytes at its output path.
        output.write_text("incomplete download")
        return 22
    shutil.copyfile(source, output)
    return 0


class InstallerTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="tide-skill-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.fixtures = self.root / "responses"
        self.fixtures.mkdir()
        self.source = self.root / "source"
        self.source.mkdir()
        for directory in ("docs", "skills"):
            shutil.copytree(REPO / directory, self.source / directory)
        for filename in ("README.md", "LICENSE", "NOTICE", "CMakeLists.txt"):
            shutil.copyfile(REPO / filename, self.source / filename)
        (self.source / "CMakeLists.txt").write_text("project(tide VERSION 9.8.7 LANGUAGES CXX)\n")
        self.project = self.root / "existing project"
        self.project.mkdir()
        self.originals = {
            "Anchor.toml": b"[provider]\ncluster = 'devnet'\n",
            "package.json": b'{"private":true}\n',
            ".env": b"EXAMPLE=unchanged\n",
            ".gitignore": b"target/\n",
        }
        for filename, content in self.originals.items():
            (self.project / filename).write_bytes(content)
        self.destination = self.root / "agent skills"
        self.skill = self.destination / "tide"
        self.lock = self.destination / ".tide-install.lock"
        self.bin = self.root / "bin"
        self.bin.mkdir()
        shim = self.bin / "curl"
        shim.write_text(
            "#!/bin/sh\nexec " + shlex.quote(sys.executable) + " "
            + shlex.quote(str(Path(__file__).resolve())) + ' --fake-curl "$@"\n'
        )
        shim.chmod(0o755)
        self.env = dict(os.environ)
        self.env.update({
            "PATH": str(self.bin) + os.pathsep + os.environ["PATH"],
            "TMPDIR": str(self.root),
            "TIDE_TEST_FIXTURES": str(self.fixtures),
        })
        self.archive(PIN)

    def archive(self, ref, extra=None):
        filename = self.fixtures / (ref.replace("/", "_") + ".tar.gz")
        with tarfile.open(filename, "w:gz", format=tarfile.USTAR_FORMAT) as archive:
            archive.add(self.source, arcname="tide-" + ref.split("/")[-1])
            if extra:
                archive.addfile(*extra)
        return filename

    def run_installer(self, *arguments, pin=PIN, success=True):
        env = dict(self.env)
        env.pop("TIDE_SKILL_COMMIT", None)
        if pin is not None:
            env["TIDE_SKILL_COMMIT"] = pin
        result = subprocess.run(
            [os.environ.get("TIDE_TEST_SHELL", "/bin/sh"), str(REPO / "install-skill.sh"),
             "--skills-dir", str(self.destination), *arguments],
            cwd=self.project, env=env, capture_output=True, text=True, timeout=30,
        )
        if success:
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stderr)
        self.assertEqual({p.name: p.read_bytes() for p in self.project.iterdir()}, self.originals)
        return result

    def assert_docs_match(self):
        expected = {p.relative_to(self.source / "docs"): p.read_bytes()
                    for p in (self.source / "docs").rglob("*") if p.is_file()}
        actual = {p.relative_to(self.skill / "references/docs"): p.read_bytes()
                  for p in (self.skill / "references/docs").rglob("*") if p.is_file()}
        self.assertEqual(actual, expected)

    def installed_files(self):
        return {p.relative_to(self.skill): p.read_bytes()
                for p in self.skill.rglob("*") if p.is_file()}

    def test_default_install_uses_maintainer_pin_without_warning(self):
        declaration = next(line for line in (REPO / "install-skill.sh").read_text().splitlines()
                           if line.startswith("PINNED_COMMIT="))
        default_pin = declaration.split("=", 1)[1].strip("'")
        self.assertRegex(default_pin, r"^[0-9a-f]{40}$")
        self.archive(default_pin)
        result = self.run_installer(pin=None)
        self.assertNotIn("warning:", result.stderr)
        self.assertIn("commit=" + default_pin, (self.skill / "SOURCE").read_text())
        self.assertIn("fallback=false", (self.skill / "SOURCE").read_text())
        requests = [json.loads(line) for line in (self.fixtures / "requests.jsonl").read_text().splitlines()]
        self.assertEqual(len(requests), 1)
        self.assertIn("https://github.com/paoloanzn/tide/archive/" + default_pin + ".tar.gz", requests[0])

    def test_installs_full_snapshot_and_preserves_reference_links(self):
        (self.source / "docs" / "additional.md").write_text("Dynamically included.\n")
        self.archive(PIN)
        self.run_installer()
        self.assertTrue(self.skill.is_dir())
        self.assertFalse(self.skill.is_symlink())
        self.assertEqual((self.skill / "SKILL.md").read_bytes(),
                         (self.source / "skills/tide/SKILL.md").read_bytes())
        self.assert_docs_match()
        for filename in ("README.md", "LICENSE", "NOTICE"):
            self.assertEqual((self.skill / "references" / filename).read_bytes(),
                             (self.source / filename).read_bytes())
        provenance = (self.skill / "SOURCE").read_text()
        self.assertIn("commit=" + PIN, provenance)
        self.assertIn("fallback=false", provenance)
        self.assertIn("tide_version=9.8.7", provenance)
        self.assertFalse(self.lock.exists())

    def test_repeated_install_keeps_existing_bundle(self):
        self.run_installer()
        before = self.skill.stat().st_ino
        mtime = (self.skill / "SKILL.md").stat().st_mtime_ns
        result = self.run_installer()
        self.assertIn("already up to date", result.stderr)
        self.assertEqual(self.skill.stat().st_ino, before)
        self.assertEqual((self.skill / "SKILL.md").stat().st_mtime_ns, mtime)
        self.assertFalse(self.lock.exists())

    def test_update_replaces_skill_and_docs_and_removes_old_pages(self):
        (self.source / "docs/old.md").write_text("old")
        self.archive(PIN)
        self.run_installer()
        (self.source / "docs/old.md").unlink()
        (self.source / "docs/new.md").write_text("new")
        with (self.source / "skills/tide/SKILL.md").open("a") as skill:
            skill.write("\nUpdated instructions.\n")
        self.archive(NEXT)
        self.run_installer(pin=NEXT)
        self.assert_docs_match()
        self.assertIn("Updated instructions.", (self.skill / "SKILL.md").read_text())
        self.assertFalse(self.lock.exists())

    def test_missing_pin_falls_back_to_resolved_main(self):
        (self.fixtures / (PIN + ".tar.gz")).unlink()
        (self.fixtures / "main-sha").write_text(MAIN)
        self.archive(MAIN)
        result = self.run_installer()
        self.assertIn("falling back to main", result.stderr)
        provenance = (self.skill / "SOURCE").read_text()
        self.assertIn("commit=" + MAIN, provenance)
        self.assertIn("fallback=true", provenance)
        self.assert_docs_match()

    def test_empty_pin_uses_main_and_records_unresolved_api(self):
        self.archive("refs/heads/main")
        result = self.run_installer(pin="")
        self.assertIn("pin 'absent'", result.stderr)
        self.assertIn("commit=main (unresolved)", (self.skill / "SOURCE").read_text())

    def test_incomplete_pin_falls_back_as_one_whole_snapshot(self):
        (self.source / "skills/tide/SKILL.md").unlink()
        self.archive(PIN)
        shutil.copyfile(REPO / "skills/tide/SKILL.md", self.source / "skills/tide/SKILL.md")
        (self.source / "docs/main-only.md").write_text("main content")
        (self.fixtures / "main-sha").write_text(MAIN)
        self.archive(MAIN)
        self.run_installer()
        self.assert_docs_match()
        self.assertIn("commit=" + MAIN, (self.skill / "SOURCE").read_text())

    def test_download_failure_keeps_installed_bundle(self):
        self.run_installer()
        before = self.installed_files()
        provenance = (self.skill / "SOURCE").read_bytes()
        self.run_installer(pin=NEXT, success=False)
        self.assertEqual(self.installed_files(), before)
        self.assertEqual((self.skill / "SOURCE").read_bytes(), provenance)

    def test_first_install_failure_creates_no_skill(self):
        self.run_installer(pin=NEXT, success=False)
        self.assertFalse(self.skill.exists())
        self.assertFalse(self.lock.exists())

    def test_corrupt_archive_keeps_installed_bundle(self):
        self.run_installer()
        before = self.installed_files()
        (self.fixtures / (NEXT + ".tar.gz")).write_bytes(b"not an archive")
        self.run_installer(pin=NEXT, success=False)
        self.assertEqual(self.installed_files(), before)

    def test_unsafe_archives_are_rejected_before_extraction(self):
        self.run_installer()
        before = self.installed_files()
        for kind in ("traversal", "absolute", "symlink", "hardlink", "fifo"):
            with self.subTest(kind=kind):
                member = tarfile.TarInfo("tide-" + NEXT + "/docs/unsafe")
                data = None
                if kind in ("traversal", "absolute"):
                    member.name = ("tide-" + NEXT + "/../../escaped" if kind == "traversal"
                                   else str(self.root / "escaped"))
                    member.size = 3
                    data = io.BytesIO(b"bad")
                else:
                    member.type = {"symlink": tarfile.SYMTYPE, "hardlink": tarfile.LNKTYPE,
                                   "fifo": tarfile.FIFOTYPE}[kind]
                    member.linkname = str(self.project)
                self.archive(NEXT, (member, data))
                self.run_installer(pin=NEXT, success=False)
                self.assertEqual(self.installed_files(), before)
                self.assertFalse((self.root / "escaped").exists())

    def test_unrelated_destination_is_preserved(self):
        self.skill.mkdir(parents=True)
        (self.skill / "SKILL.md").write_text("User's own skill")
        result = self.run_installer(success=False)
        self.assertIn("unrelated skill", result.stderr)
        self.assertEqual((self.skill / "SKILL.md").read_text(), "User's own skill")

    def test_unrelated_symlink_is_preserved(self):
        self.destination.mkdir()
        self.skill.symlink_to(self.root / "missing")
        self.run_installer(success=False)
        self.assertEqual(self.skill.readlink(), self.root / "missing")

    def test_all_destinations_checked_before_registering_any(self):
        second = self.root / "second"
        (second / "tide").mkdir(parents=True)
        self.run_installer("--skills-dir", str(second), success=False)
        self.assertFalse(self.skill.exists())
        self.assertTrue((second / "tide").is_dir())

    def test_multiple_destinations_receive_identical_updates(self):
        second = self.root / "second"
        self.run_installer("--skills-dir", str(second))
        self.assertFalse((second / "tide").is_symlink())
        self.assertEqual((self.skill / "SOURCE").read_bytes(), (second / "tide/SOURCE").read_bytes())
        self.archive(NEXT)
        self.run_installer("--skills-dir", str(second), pin=NEXT)
        self.assertEqual((self.skill / "SOURCE").read_bytes(), (second / "tide/SOURCE").read_bytes())
        self.assertIn("commit=" + NEXT, (second / "tide/SOURCE").read_text())

    def fail_activation_at(self, destination):
        shim = self.bin / "mv"
        shim.write_text(
            '#!/bin/sh\nif [ "$2" = '
            + shlex.quote(str(destination / ".tide-install.lock/new"))
            + ' ]; then exit 1; fi\nexec ' + shlex.quote(shutil.which("mv")) + ' "$@"\n'
        )
        shim.chmod(0o755)

    def test_failed_activation_rolls_back_all_destinations(self):
        self.run_installer()
        before = self.installed_files()
        self.archive(NEXT)
        second = self.root / "second"
        self.fail_activation_at(second)
        self.run_installer("--skills-dir", str(second), pin=NEXT, success=False)
        self.assertEqual(self.installed_files(), before)
        self.assertFalse((second / "tide").exists())
        self.assertFalse((second / ".tide-install.lock").exists())
        self.assertFalse(self.lock.exists())

    def test_failed_activation_restores_its_own_backup(self):
        self.run_installer()
        before = self.installed_files()
        self.archive(NEXT)
        self.fail_activation_at(self.destination)
        self.run_installer(pin=NEXT, success=False)
        self.assertEqual(self.installed_files(), before)
        self.assertFalse(self.lock.exists())

    def test_termination_after_rename_restores_previous_bundle(self):
        self.run_installer()
        before = self.installed_files()
        self.archive(NEXT)
        shim = self.bin / "mv"
        shim.write_text(
            "#!/bin/sh\n" + shlex.quote(shutil.which("mv")) + ' "$@" || exit $?\n'
            + 'if [ "$2" = ' + shlex.quote(str(self.lock / "new"))
            + ' ]; then kill -TERM "$PPID"; fi\n'
        )
        shim.chmod(0o755)
        self.run_installer(pin=NEXT, success=False)
        self.assertEqual(self.installed_files(), before)
        self.assertFalse(self.lock.exists())

    def test_existing_lock_is_respected(self):
        self.run_installer()
        self.lock.mkdir()
        result = self.run_installer(success=False)
        self.assertIn("another install may be running", result.stderr)
        self.assertTrue(self.lock.is_dir())

    def test_invalid_pin_is_rejected_without_download(self):
        self.run_installer(pin="../main", success=False)
        self.assertFalse((self.fixtures / "requests.jsonl").exists())

    def test_help_has_no_side_effects(self):
        result = subprocess.run(["sh", str(REPO / "install-skill.sh"), "--help"],
                                env=self.env, capture_output=True, text=True, check=True)
        self.assertIn("Usage:", result.stdout)
        self.assertFalse(self.destination.exists())
        self.assertFalse((self.fixtures / "requests.jsonl").exists())


if __name__ == "__main__":
    if sys.argv[1:2] == ["--fake-curl"]:
        sys.exit(fake_curl(sys.argv[2:]))
    unittest.main()
