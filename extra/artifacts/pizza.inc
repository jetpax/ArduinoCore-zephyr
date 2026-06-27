# Copyright (c) 2026 Jonathan Elliot Peace <jep@alphabetiq.com>
# SPDX-License-Identifier: Apache-2.0

# pizza-specific files to include in the core package, beyond _common.inc.

# The CDC uploader is referenced by platform.txt as
# {runtime.platform.path}/tools/cdc-upload/cdc-upload.py, so it must ship inside
# the core package. (The other tools/ entries are Go sources / separate
# boards-manager tool dependencies, not part of the core archive.)
tools/cdc-upload/
