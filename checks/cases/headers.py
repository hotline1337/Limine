import os
import subprocess

from harness.api import check
from harness.env import SRCDIR
from cases.tsc import build_cc


@check(ports=['bios'], tags=['ci'])
def config_get_value_next_header(t):
    """config_get_value_next() declaration is reachable and correct."""
    source = '#include <lib/config.h>\n'
    command = build_cc(t) + ['-std=gnu11', '-ffreestanding', '-Wall', '-Wextra', '-Werror',
                             '-DBIOS', '-I', os.path.join(SRCDIR, 'common'),
                             '-fsyntax-only', '-x', 'c', '-']
    result = subprocess.run(command, input=source.encode(), stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    t.check(result.returncode == 0,
            'compiling lib/config.h failed:\n' + result.stdout.decode(errors='replace'))


@check(ports=['bios'], tags=['ci'])
def efi_headers_with_linux_macro(t):
    """UEFI interfaces compile with the Linux predefined macro present."""
    source = ('#include <lib/misc.h>\n#include <lib/tpm.h>\n#include <lib/uri.h>\n'
              '#include <protos/chainload.h>\n#include <protos/linux_efi.h>\n')
    command = build_cc(t) + [
        '-std=gnu11', '-ffreestanding', '-Wall', '-Wextra', '-Wshadow',
        '-Wvla', '-Werror', '-fshort-wchar', '-DUEFI', '-Dlinux=1', '-nostdinc',
        '-I', os.path.join(SRCDIR, 'picoefi', 'include'),
        '-I', os.path.join(SRCDIR, 'common'),
        '-I', os.path.join(SRCDIR, 'common', 'libc-compat'),
        '-I', os.path.join(SRCDIR, 'limine-protocol', 'include'),
        '-I', os.path.join(SRCDIR, 'flanterm', 'src'),
        '-isystem', os.path.join(SRCDIR, 'freestanding-c-hdrs', 'include'),
        '-fsyntax-only', '-x', 'c', '-',
    ]
    result = subprocess.run(command, input=source.encode(), stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    t.check(result.returncode == 0,
            'compiling EFI headers failed:\n' + result.stdout.decode(errors='replace'))
