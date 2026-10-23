import os
import random

import nas_sync


def table(tmp_path, *entries):
    """A mountinfo file holding `entries`, each a (mount point, fstype, source)."""
    lines = ["%d 1 0:%d / %s rw,relatime shared:%d - %s %s rw\n"
             % (40 + i, 30 + i, point, i, fstype, source)
             for i, (point, fstype, source) in enumerate(entries)]
    path = tmp_path / "mountinfo"
    path.write_text("".join(lines))
    return str(path)


def share(tmp_path):
    """A mount point that exists, named at random, as the kernel would print it."""
    point = tmp_path / ("ionozond_%dtb" % random.randint(2, 99))
    point.mkdir()
    return os.path.realpath(str(point))


def test_fired_automount_resolves_to_the_share_stacked_on_it(tmp_path):
    point = share(tmp_path)
    source = "//10.%d.%d.%d/archive" % tuple(random.randint(1, 254) for _ in range(3))
    mounts = table(tmp_path, ("/", "ext4", "/dev/sda2"), (point, "autofs", "systemd-1"), (point, "cifs", source))
    assert nas_sync.mount_entry_for(point, mounts) == (point, "cifs"), "a share mounted over its automount trigger was read as the trigger"


def test_unfired_automount_still_resolves_to_its_trigger(tmp_path):
    point = share(tmp_path)
    mounts = table(tmp_path, ("/", "ext4", "/dev/nvme0n1p%d" % random.randint(1, 9)), (point, "autofs", "systemd-1"))
    assert nas_sync.mount_entry_for(point, mounts) == (point, "autofs"), "an automount with nothing behind it was not read as a bare trigger"
