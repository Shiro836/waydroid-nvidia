#
# Copyright (C) 2021 The Waydroid project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

top_dir=`pwd`
LOCALDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
loc_man="${top_dir}/.repo/local_manifests"
manifests_url="https://raw.githubusercontent.com/waydroid/android_vendor_waydroid/lineage-20/manifest_scripts/manifests"
manifests_path="${LOCALDIR}/manifests"

#setup colors
red=`tput setaf 1`
green=`tput setaf 2`
yellow=`tput setaf 3`
blue=`tput setaf 4`
purple=`tput setaf 5`
teal=`tput setaf 6`
light=`tput setaf 7`
dark=`tput setaf 8`
ltred=`tput setaf 9`
ltgreen=`tput setaf 10`
ltyellow=`tput setaf 11`
ltblue=`tput setaf 12`
ltpurple=`tput setaf 13`
CL_CYN=`tput setaf 12`
CL_RST=`tput sgr0`
reset=`tput sgr0`

if [ ! -d "${top_dir}/.repo" ]; then
    echo -e ${reset}""${reset}
    echo -e ${ltred}"ERROR: Manifest generation requires repo to be initialized first."${reset}
    echo -e ${reset}""${reset}
    exit
fi

if [ ! -f build/make/core/version_defaults.mk ]; then
    echo -e ${reset}""${reset}
    echo -e ${ltred}"ERROR: Missing build/make. Please run `repo sync build/make` first."${reset}
    echo -e ${reset}""${reset}
    exit
fi

sdkv=$(cat build/make/core/version_defaults.mk | grep "PLATFORM_SDK_VERSION :=" | grep -o "[[:digit:]]\+")
manifests_url="${manifests_url}-${sdkv}"
manifests_path="${manifests_path}-${sdkv}"

mkdir -p ${loc_man}

echo -e ${reset}""${reset}
echo -e ${green}"Placing manifest fragments..."${reset}
echo -e ${reset}""${reset}
if [ -d "${manifests_path}" ]; then
    cp -fpr ${manifests_path}/*.xml "${loc_man}/"
else
    echo -e ${reset}""${reset}
    echo -e ${ltblue}"INFO: Manifests not found, downloading"${reset}
    echo -e ${reset}""${reset}
    wget "${manifests_url}/00-remotes.xml" -O "${loc_man}/00-remotes.xml"
    wget "${manifests_url}/01-removes.xml" -O "${loc_man}/01-removes.xml"
    wget "${manifests_url}/02-waydroid.xml" -O "${loc_man}/02-waydroid.xml"
fi

echo -e ${reset}""${reset}
echo -e ${teal}"INFO: Cleaning up remove manifest entries"${reset}
echo -e ${reset}""${reset}
while IFS= read -r rpitem; do
    if [[ $rpitem == *"remove-project"* ]]; then
        rpitem_trimmed="$(echo "$rpitem" | xargs)"
        if grep -qRlZ "$rpitem_trimmed" "${top_dir}/.repo/manifests/"; then
            echo -e ${yellow}"WARN: ROM already includes: $rpitem"${reset}
        else
            echo -e ${green}"INFO: Needed: $rpitem"${reset}
            prefix="<remove-project name="
            suffix=" />"
            item=${rpitem_trimmed#"$prefix"}
            item=${item%"$suffix"}
            if ! grep -qRlZ "$item" "${top_dir}/.repo/manifests/"; then
                sed -e "$item"'d' "${loc_man}/01-removes.xml"
            fi
        fi
    fi
done < "${loc_man}/01-removes.xml"
