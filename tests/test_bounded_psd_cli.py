"""Public PSD composite regressions. Python 3.9+, standard library only."""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
import zlib

from psd_fixtures import (HEIGHT, LAYER_RGB, PREDICTED_ZIP, RAW, RLE, WIDTH, ZIP,
                          layer_info, packbits, pixels, predict, prefix, read_png,
                          rgb_profile, small_psd, srgb_encode, version_info,
                          write_stress_psd, write_tiff)


PROJECT_CONFIG = """ocio_profile_version: 2
roles:
  scene_linear: working
file_rules:
  - !<Rule> {name: Default, colorspace: working}
displays:
  Preview:
    - !<View> {name: Standard, colorspace: Project Output}
colorspaces:
  - !<ColorSpace> {name: working, aliases: [linear, lin_rec709_scene], bitdepth: 32f, isdata: false}
  - !<ColorSpace>
    name: Project Output
    aliases: [sRGB]
    bitdepth: 32f
    isdata: false
    from_scene_reference: !<ExponentTransform> {value: [0.5, 0.5, 0.5, 1]}
"""


def fitted(width, height, size):
    scale = min(1, size[0] / width, size[1] / height)
    return max(1, int(width * scale + .5)), max(1, int(height * scale + .5))


def srgb_from_gamma(value, gamma):
    return srgb_encode(value ** gamma)


def oracle(depth, alpha, transfer=lambda value: value):
    maximum = (1 << depth) - 1
    for sample in pixels(depth, alpha):
        rgb = tuple(round(255 * transfer(value / maximum)) for value in sample[:3])
        if alpha:
            a = round(255 * sample[3] / maximum)
            yield (*(rgb if a else (0, 0, 0)), a)
        else:
            yield rgb


def check_fixture_encoding():
    assert packbits(bytes(128) + b"\xde\xad") == b"\x81\0\x01\xde\xad"
    increasing = bytes(range(129))
    assert packbits(increasing) == b"\x7f" + increasing[:128] + b"\0\x80"
    assert predict([250, 3, 255, 0], 8) == bytes((250, 9, 252, 1))
    assert predict([0xfffd, 3, 0xab02, 0x00ff], 16).hex() == "fffd0006aaff55fd"
    for depth in (8, 16):
        for channels in ("rgb", "rgba", "spot", "aux"):
            for codec in (RAW, RLE, ZIP, PREDICTED_ZIP):
                data = small_psd(depth, codec, channels)
                assert struct.unpack_from(">4sH6xHIIHH", data) == (
                    b"8BPS", 1, 3 if channels == "rgb" else 4, HEIGHT, WIDTH, depth, 3)
                offset = 26
                for _ in range(3):
                    offset += 4 + struct.unpack_from(">I", data, offset)[0]
                assert struct.unpack_from(">H", data, offset)[0] == codec
                payload = data[offset + 2:]
                row_count = HEIGHT * (3 if channels == "rgb" else 4)
                byte_count = WIDTH * row_count * (depth // 8)
                if codec == RAW:
                    assert len(payload) == byte_count
                elif codec == RLE:
                    sizes = struct.unpack_from(f">{row_count}H", payload)
                    assert all(sizes) and sum(sizes) + 2 * row_count == len(payload)
                else:
                    assert len(zlib.decompress(payload)) == byte_count
    assert list(oracle(8, True))[:3] == [(0, 0, 0, 0), (255, 0, 255, 64), (255, 0, 255, 128)]
    print("PSD fixture encodings verified")


class CLI(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix="thumbgen-psd-")
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)

    def fixture(self, name, data):
        path = self.root / (name + ".psd")
        path.write_bytes(data)
        return path

    def command(self, *args, status=0, ocio=None):
        command = [str(EXE), *map(str, args), "--json"]
        env = ENV if ocio is None else dict(ENV, OCIO=str(ocio))
        run = subprocess.run(command, env=env, capture_output=True, text=True, encoding="utf-8", timeout=60)
        try:
            records = [json.loads(line) for line in run.stdout.splitlines()]
        except json.JSONDecodeError as error:
            self.fail(f"{command}: invalid JSON ({error})\n{run.stdout}\n{run.stderr}")
        failures = [(record.get("id"), record["error"]) for record in records if "error" in record]
        self.assertEqual(run.returncode, status, (command, failures, run.stderr))
        self.assertTrue(records and all(record["schema_version"] == 1 for record in records), records)
        return records

    def convert(self, path, name, *args, size=(WIDTH, HEIGHT), status=0, ocio=None):
        output = self.root / (name + ".png")
        records = self.command("convert", path, "--output", output, "--size", f"{size[0]}x{size[1]}",
                               "--format", "png", "--background", "transparent", "--threads", 1,
                               "--thread-budget", 1, *args, status=status, ocio=ocio)
        result = records[0]
        self.assertEqual(result["type"], "result", records)
        self.assertEqual(result["ok"], status == 0, result)
        self.assertEqual(output.exists(), status == 0, result)
        return result, output

    def job(self, name, path, sizes, **options):
        return dict(id=name, input=str(path), options=options, outputs=[
            dict(path=str(self.root / f"{name}-{index}.png"), size=list(size), format="png")
            for index, size in enumerate(sizes)])

    def batch(self, jobs, status=0, budget=512):
        manifest = self.root / "jobs.json"
        manifest.write_text(json.dumps(dict(schema_version=1,
            defaults=dict(threads=1, background="transparent"),
            execution=dict(workers=1, thread_budget=1, memory_budget_mib=budget),
            jobs=jobs)), encoding="utf-8")
        records = self.command("batch", "--manifest", manifest, status=status)
        results = [record for record in records if record["type"] == "result"]
        self.assertEqual([result["id"] for result in results], [job["id"] for job in jobs], records)
        self.assertEqual(records[-1]["type"], "summary", records)
        return results

    def check_pixels(self, actual, expected, tolerance=0):
        expected = list(expected)
        self.assertEqual(len(actual), len(expected))
        for i, (a, b) in enumerate(zip(actual, expected)):
            # PNG writers may omit a fully opaque alpha channel.
            self.assertEqual(a[3] if len(a) == 4 else 255, b[3] if len(b) == 4 else 255, (i, a, b))
            if max(abs(x - y) for x, y in zip(a[:3], b[:3])) > tolerance:
                self.fail(f"pixel {i}: got {a}, expected {b}, tolerance {tolerance}")

    def output_pixels(self, result, job, dimensions):
        self.assertTrue(result["ok"], result)
        self.assertEqual(len(result["outputs"]), len(job["outputs"]), result)
        self.assertGreater(result["execution"]["admission_mib"], 0, result)
        images = []
        for output, request in zip(result["outputs"], job["outputs"]):
            size = fitted(*dimensions, request["size"])
            self.assertEqual(output["status"], "success", output)
            self.assertEqual(Path(output["path"]), Path(request["path"]), output)
            self.assertEqual((output["width"], output["height"]), size, output)
            width, height, image = read_png(Path(output["path"]))
            self.assertEqual((width, height), size, output)
            images.append(image)
        return images

    def test_legacy_inspect_and_part_selection(self):
        source = self.fixture("legacy", small_psd(layers=layer_info(8, 1)))
        inspection = self.command("inspect", source)[0]
        self.assertEqual(inspection["type"], "inspection", inspection)
        self.assertTrue(inspection["ok"], inspection)
        self.assertEqual([(part["part"], part["name"], part["width"], part["height"], part["channels"])
                          for part in inspection["parts"]],
                         [(0, "", WIDTH, HEIGHT, ["R", "G", "B"]), (1, "Layer", 2, 2, ["R", "G", "B"])])
        for name, opts in (("index", ("--part", 1)), ("name", ("--part-name", "Layer"))):
            result, output = self.convert(source, name, *opts)
            self.assertEqual(result["selection"]["chosen"]["part"], 1, result)
            width, height, image = read_png(output)
            self.assertEqual((width, height), (2, 2))
            self.check_pixels(image, [LAYER_RGB] * 4, 1)
        result, _ = self.convert(source, "absent", "--part", 9, status=4)
        self.assertEqual(result["error"]["code"], "part_not_found", result)
        result, output = self.convert(source, "composite", "--part", 0)
        self.assertEqual(result["selection"]["chosen"]["part"], 0, result)
        self.check_pixels(read_png(output)[2], oracle(8, False), 1)

    def test_baseline_raw_and_rle_rgb(self):
        jobs = [self.job(name, self.fixture(name, small_psd(depth, codec)), [(WIDTH, HEIGHT), (173, 9)])
                for depth in (8, 16) for codec in (RAW, RLE) for name in (f"rgb-{depth}-{codec}",)]
        for result, job in zip(self.batch(jobs), jobs):
            with self.subTest(id=job["id"]):
                image = self.output_pixels(result, job, (WIDTH, HEIGHT))[0]
                depth = int(job["id"].split("-")[1])
                self.check_pixels(image, oracle(depth, False), 1)

    def test_compression_and_transparency_matrix(self):
        sizes = [(WIDTH, HEIGHT), (173, 7), (67, 6)]
        jobs, cases = [], []
        for depth in (8, 16):
            for channels in ("rgb", "rgba", "spot", "aux"):
                for codec in (RAW, RLE, ZIP, PREDICTED_ZIP):
                    name = f"{channels}-{depth}-{codec}"
                    jobs.append(self.job(name, self.fixture(name, small_psd(depth, codec, channels)), sizes))
                    cases.append((depth, channels == "rgba"))
        results = self.batch(jobs)
        for job, result, (depth, alpha) in zip(jobs, results, cases):
            with self.subTest(id=job["id"]):
                selection = result["selection"]["chosen"]
                self.assertEqual(selection["part"], 0, result)
                self.assertEqual(selection["channels"], ["R", "G", "B"] + (["A"] if alpha else []), result)
                image = self.output_pixels(result, job, (WIDTH, HEIGHT))[0]
                self.check_pixels(image, oracle(depth, alpha), 3 if depth == 8 and alpha else 1)

    def test_bounded_predicted_zip_admission(self):
        # Cross the 8 MP bounded-path threshold without a large on-disk fixture.
        width, height, depth = 4096, 2048, 16
        source = self.root / "bounded.psd"
        write_stress_psd(source, width, height, depth, PREDICTED_ZIP, True)
        job = self.job("bounded", source, [(173, 131), (64, 64)], max_decode_mib=24)
        result = self.batch([job], budget=288)[0]
        images = self.output_pixels(result, job, (width, height))
        self.assertLessEqual(result["execution"]["admission_mib"], 24, result)
        self.assertEqual(result["decoded_mib"], width * height * 8 / (1024 * 1024), result)
        for image, request in zip(images, job["outputs"]):
            ow, oh = fitted(width, height, request["size"])
            midline = image[(oh // 2) * ow:(oh // 2 + 1) * ow]
            self.assertEqual(midline[0][3], 0)
            self.assertEqual(midline[ow // 2][3], 255)
            self.assertEqual(midline[-1][3], 0)

    def test_zip_inspect_composite(self):
        source = self.fixture("inspect-zip", small_psd(16, PREDICTED_ZIP, "rgba"))
        inspection = self.command("inspect", source)[0]
        self.assertTrue(inspection["ok"], inspection)
        self.assertEqual([(part["part"], part["width"], part["height"], part["channels"])
                          for part in inspection["parts"]], [(0, WIDTH, HEIGHT, ["R", "G", "B", "A"])])
        result, output = self.convert(source, "zip-part-zero", "--part", 0)
        self.assertEqual(result["selection"]["chosen"]["part"], 0, result)
        self.check_pixels(read_png(output)[2], oracle(16, True), 1)

    def test_icc_with_lossless_reference_and_config_precedence(self):
        gamma = 1.75
        icc = rgb_profile(gamma)
        source = self.fixture("calibrated", small_psd(16, PREDICTED_ZIP, "rgba", icc=icc))
        srgb = self.fixture("srgb", small_psd(8, RAW, icc=rgb_profile()))
        invalid = self.fixture("invalid-icc", small_psd(8, ZIP, icc=icc[:40]))
        untagged = self.fixture("untagged", small_psd())
        config = self.root / "project.ocio"
        config.write_text(PROJECT_CONFIG, encoding="utf-8")
        for name, path, opts, inherited, expected, tolerance in (
            ("implicit", source, (), None, oracle(16, True, lambda v: srgb_from_gamma(v, gamma)), 2),
            ("inherited", source, (), config, oracle(16, True, lambda v: srgb_from_gamma(v, gamma)), 2),
            ("builtin", source, ("--ocio-config", "ocio://default"), config,
             oracle(16, True, lambda v: srgb_from_gamma(v, gamma)), 2),
            ("srgb", srgb, (), None, oracle(8, False), 1),
        ):
            with self.subTest(case=name):
                result, output = self.convert(path, name, *opts, ocio=inherited)
                colour = result["colour"]
                self.assertEqual(colour["basis"], "embedded_icc", result)
                self.assertEqual(colour["icc"]["model"], "RGB", result)
                self.assertEqual(colour["config_basis"], "explicit" if opts else "builtin", result)
                self.assertEqual(colour["ocio_config"], "ocio://default", result)
                self.check_pixels(read_png(output)[2], expected, tolerance)

        sizes = [(WIDTH, HEIGHT), (173, 7), (67, 6)]
        jobs = [self.job("psd", source, sizes)]
        for endian, name in (("<", "little"), (">", "big")):
            tiff = self.root / (name + ".tiff")
            write_tiff(tiff, WIDTH, HEIGHT, 16, pixels(16, True), endian, icc)
            jobs.append(self.job(name, tiff, sizes))
        psd, little, big = [self.output_pixels(result, job, (WIDTH, HEIGHT))
                            for result, job in zip(self.batch(jobs), jobs)]
        for a, b, c in zip(psd, little, big):
            self.check_pixels(b, c)
            self.check_pixels(a, b, 2)

        result, _ = self.convert(invalid, "rejected-icc", status=4)
        self.assertEqual(result["error"]["code"], "colour_input_unsupported", result)
        result, _ = self.convert(source, "rejected-bridge", "--ocio-config", config, status=4)
        self.assertEqual(result["error"]["code"], "colour_input_unsupported", result)
        self.assertIn("bridge", result["error"]["message"].lower())
        for name, path, opts, inherited, basis, transfer in (
            ("override-invalid", invalid, ("--input-space", "sRGB"), None, "builtin", lambda v: v),
            ("explicit-config", invalid, ("--input-space", "linear", "--ocio-config", config),
             None, "explicit", lambda v: v ** .5),
            ("inherited-config", untagged, ("--input-space", "linear"), config, "environment", lambda v: v ** .5),
        ):
            with self.subTest(case=name):
                result, output = self.convert(path, name, *opts, ocio=inherited)
                self.assertEqual(result["colour"]["basis"], "explicit", result)
                self.assertEqual(result["colour"]["config_basis"], basis, result)
                self.assertNotIn("icc", result["colour"])
                self.check_pixels(read_png(output)[2], oracle(8, False, transfer), 2)

    def test_corrupt_composites_do_not_publish(self):
        byte_count = WIDTH * HEIGHT * 3
        raw = small_psd()
        rle = small_psd(compression=RLE)
        zip_file = small_psd(compression=ZIP)
        predicted = small_psd(compression=PREDICTED_ZIP)
        header = prefix(WIDTH, HEIGHT, 8, 3)
        rle_table = len(header) + 2
        rle_offset = rle_table + 2 * 3 * HEIGHT
        first_row_length = struct.unpack_from(">H", rle, rle_table)[0]
        overflow = b"\x81\xff" * 3  # Three repeats of 128 exceed the 259-byte scanline.
        bad_run = (rle[:rle_table] + struct.pack(">H", len(overflow)) + rle[rle_table + 2:rle_offset]
                   + overflow + rle[rle_offset + first_row_length:])
        missing = prefix(WIDTH, HEIGHT, 8, 3, version_info(False))
        bad_resource_length = (prefix(WIDTH, HEIGHT, 8, 3, b"8BIM\x04\x0f\0\0\0\0\0\x20")
                               + raw[len(header):])
        cases = {
            "section-offset": (raw[:30] + struct.pack(">I", len(raw)) + raw[34:], "open_failed"),
            "resource-offset": (bad_resource_length, "open_failed"),
            "layer-offset": (raw[:34] + struct.pack(">I", len(raw)) + raw[38:], "open_failed"),
            "little-endian-header": (struct.pack("<4sH6xHIIHH", b"8BPS", 1, 3, HEIGHT, WIDTH, 8, 3)
                                     + raw[26:], "open_failed"),
            "no-composite": (header + b"\0\0", "decode_failed"),
            "unmerged-placeholder": (missing + raw[len(header):], "decode_failed"),
            "raw-short": (raw[:-1], "decode_failed"),
            "rle-short-table": (rle[:rle_offset - 1], "decode_failed"),
            "rle-short-file": (rle[:-1], "decode_failed"),
            "rle-row-overflow": (bad_run, "decode_failed"),
            "zip-bad-zlib": (zip_file[:-1] + bytes((zip_file[-1] ^ 1,)), "decode_failed"),
            "zip-truncated": (zip_file[:-3], "decode_failed"),
            "predicted-truncated": (predicted[:-3], "decode_failed"),
            "zip-extra-sample": (header + b"\0\2" + zlib.compress(bytes(byte_count + 1)), "decode_failed"),
            "zip-short-image": (header + b"\0\2" + zlib.compress(bytes(byte_count - 1)), "decode_failed"),
            "zip-trailing-stream": (zip_file + zlib.compress(b""), "decode_failed"),
        }
        # Check a valid job after the errors, including after the inflater jobs,
        # so a failed job cannot leave a reused worker or batch in a broken state.
        jobs = [self.job(name, self.fixture(name, data), [(WIDTH, HEIGHT), (80, 9)])
                for name, (data, _) in cases.items()]
        jobs.append(self.job("valid", self.fixture("valid", predicted), [(WIDTH, HEIGHT)]))
        results = self.batch(jobs, status=3)
        for job, result in zip(jobs[:-1], results[:-1]):
            with self.subTest(id=job["id"]):
                self.assertFalse(result["ok"], result)
                self.assertEqual(result["error"]["code"], cases[job["id"]][1], result)
                self.assertFalse(any(Path(output["path"]).exists() for output in job["outputs"]), result)
        self.check_pixels(self.output_pixels(results[-1], jobs[-1], (WIDTH, HEIGHT))[0], oracle(8, False), 1)

    def test_budget_refusal_does_not_publish(self):
        source = self.fixture("budget", small_psd(16, PREDICTED_ZIP, "rgba", icc=rgb_profile(1.75)))
        result, _ = self.convert(source, "decode-refused", "--max-decode-mib", 1, status=4)
        self.assertEqual(result["error"]["code"], "decode_limit_exceeded", result)
        result, _ = self.convert(source, "admission-refused", "--memory-budget-mib", 265, status=4)
        self.assertEqual(result["error"]["code"], "task_memory_exceeds_budget", result)
        self.assertGreater(result["execution"]["admission_mib"], 1, result)


def stress_job(command, timeout):
    """Measure the Linux worker high-water RSS when procfs exposes children."""
    with subprocess.Popen(command, env=ENV, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          text=True, encoding="utf-8") as process:
        done, peak_kib = threading.Event(), 0

        def sample():
            nonlocal peak_kib
            tasks = Path(f"/proc/{process.pid}/task")
            while not done.wait(.05):
                pids = set()
                for children in tasks.glob("*/children"):
                    try:
                        pids.update(children.read_text().split())
                    except OSError:
                        continue
                for pid in pids:
                    try:
                        status = Path(f"/proc/{pid}/status").read_text()
                    except OSError:
                        continue
                    for line in status.splitlines():
                        if line.startswith("VmHWM:"):
                            peak_kib = max(peak_kib, int(line.split()[1]))

        monitor = threading.Thread(target=sample)
        if sys.platform.startswith("linux"):
            monitor.start()
        try:
            stdout, stderr = process.communicate(timeout=timeout + 20)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()
            raise
        finally:
            done.set()
            if monitor.ident is not None:
                monitor.join()
        assert process.returncode == 0, (command, stdout, stderr)
        result = json.loads(stdout.splitlines()[0])
        return result, peak_kib / 1024 if peak_kib else None


def run_stress(task_timeout):
    sizes = [(12000, 6000, 8, False), (16000, 8000, 16, True)]
    with tempfile.TemporaryDirectory(prefix="thumbgen-psd-stress-") as tmp:
        root = Path(tmp)
        source = root / "synthetic.psd"
        for width, height, depth, alpha in sizes:
            reference = None
            for codec in (RAW, RLE, ZIP, PREDICTED_ZIP):
                write_stress_psd(source, width, height, depth, codec, alpha)
                previews = []
                for budget in (512, 2048):
                    output = root / f"preview-{budget}.png"
                    command = [str(EXE), "convert", str(source), "--output", str(output), "--format", "png",
                               "--size", "720x490", "--background", "transparent", "--threads", "1",
                               "--thread-budget", "1", "--memory-budget-mib", str(budget),
                               "--max-decode-mib", str(budget), "--task-timeout-ms", str(task_timeout * 1000),
                               "--json"]
                    result, peak = stress_job(command, task_timeout)
                    execution = result["execution"]
                    print(json.dumps(dict(width=width, height=height, depth=depth, compression=codec,
                                          budget_mib=budget, admission_mib=execution.get("admission_mib"),
                                          estimated_task_mib=execution.get("estimated_task_mib"),
                                          sampled_worker_peak_mib=peak, wall_ms=execution.get("wall_ms"))), flush=True)
                    assert result["ok"] and execution["admission_mib"] + execution["fixed_reserve_mib"] <= budget, result
                    ow, oh, pixels_out = read_png(output)
                    assert (ow, oh) == fitted(width, height, (720, 490)), result
                    previews.append(pixels_out)
                    output.unlink()
                assert previews[0] == previews[1], (width, depth, codec)
                if reference is None:
                    reference = previews[0]
                assert previews[0] == reference, (width, depth, codec)
                source.unlink()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path)
    parser.add_argument("--runtime-dir", type=Path)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--fixtures-only", action="store_true")
    mode.add_argument("--stress", action="store_true", help="run only the opt-in 12K/16K matrix")
    parser.add_argument("--task-timeout-seconds", type=int, default=60, help="stress timeout per conversion")
    args, remaining = parser.parse_known_args()
    if args.fixtures_only:
        check_fixture_encoding()
    else:
        if args.exe is None or not args.exe.is_file():
            parser.error("--exe must name a ThumbGen executable")
        if args.task_timeout_seconds < 1:
            parser.error("--task-timeout-seconds must be positive")
        EXE = args.exe.resolve()
        ENV = dict(os.environ)
        ENV.pop("OCIO", None)
        if args.runtime_dir:
            key = "PATH" if os.name == "nt" else "LD_LIBRARY_PATH"
            ENV[key] = str(args.runtime_dir.resolve()) + os.pathsep + ENV.get(key, "")
        if args.stress:
            run_stress(args.task_timeout_seconds)
        else:
            unittest.main(argv=[sys.argv[0], *remaining])
