# Common definitions for all test suites.
# Copyright 2016 Serval Project Inc.
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

testdefs_sh=$(abspath "${BASH_SOURCE[0]}")
lbard_source_root="${testdefs_sh%/*}"
lbard_build_root="${testdefs_sh%/*}"

export TFW_LOGDIR="${TFW_LOGDIR:-$lbard_build_root/testlog}"
export PATH="$lbard_build_root:$PATH"

source "$lbard_source_root/serval-dna/testdefs.sh"
source "$lbard_source_root/serval-dna/testdefs_rhizome.sh"

