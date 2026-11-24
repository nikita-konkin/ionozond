import os
import random
import string
import subprocess


SCRIPT = os.path.join(os.path.dirname(__file__), "..", "..", "tools", "sounder.sh")


def launch(tmp_path, defaults, **env):
    """sounder.sh run against a fake python3 that prints the arguments it was given."""
    fake = tmp_path / "bin"
    fake.mkdir()
    (fake / "python3").write_text("#!/bin/sh\necho \"$@\"\n")
    (fake / "python3").chmod(0o755)
    (fake / "systemctl").write_text("#!/bin/sh\nexit 1\n")
    (fake / "systemctl").chmod(0o755)
    (tmp_path / "defaults").write_text(defaults)
    run = {"PATH": "%s:%s" % (fake, os.environ["PATH"]),
           "IONOZOND_DEFAULTS": str(tmp_path / "defaults")}
    run.update(env)
    return subprocess.run(["bash", SCRIPT, str(tmp_path / "chirp_config.py")],
                          env=run, capture_output=True, text=True,
                          timeout=30)


def address():
    return "10.%d.%d.%d" % tuple(random.randint(1, 254) for _ in range(3))


def test_station_without_a_receiver_choice_keeps_its_own_arguments(tmp_path):
    flag = "--threads %d" % random.randint(2, 9)
    assert flag in launch(tmp_path, "SOUNDER_ARGS=%s\n" % flag).stdout, "the station's own SOUNDER_ARGS were not passed on"


def test_station_without_any_setting_does_not_receive_on_a_hackrf(tmp_path):
    assert "--hackrf" not in launch(tmp_path, "RADIO_ADDR=%s\n" % address()).stdout, "a station that chose nothing was switched to the HackRF"


def test_hackrf_choice_replaces_the_station_arguments(tmp_path):
    out = launch(tmp_path, "SOUNDER_ARGS=--no-gpsdo\n", SOUNDER_RECEIVER="hackrf").stdout
    assert "--hackrf" in out and "--no-gpsdo" not in out, "the HackRF choice did not replace the station arguments"


def test_n210_choice_reaches_the_radio_at_the_station_address(tmp_path):
    radio = address()
    assert "--args addr=%s" % radio in launch(tmp_path, "RADIO_ADDR=%s\n" % radio, SOUNDER_RECEIVER="n210-gpsdo").stdout, "the N210 choice ignored RADIO_ADDR"


def test_n210_host_clock_choice_turns_the_gpsdo_off(tmp_path):
    assert "--no-gpsdo" in launch(tmp_path, "", SOUNDER_RECEIVER="n210").stdout, "the host clock choice left the GPSDO on"


def test_hackrf_timed_by_the_n210_reads_time_at_the_station_address(tmp_path):
    radio = address()
    assert "--time-from-usrp addr=%s" % radio in launch(tmp_path, "RADIO_ADDR=%s\n" % radio, SOUNDER_RECEIVER="hackrf-usrp-time").stdout, "the HackRF did not take its time from the N210 at RADIO_ADDR"


def test_unknown_receiver_choice_is_refused(tmp_path):
    choice = "".join(random.choice(string.ascii_lowercase) for _ in range(8))
    assert launch(tmp_path, "", SOUNDER_RECEIVER=choice).returncode != 0, "an unknown receiver choice was launched anyway"
