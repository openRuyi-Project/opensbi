#!/usr/bin/env bash
set -Eeuo pipefail

src_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build_root=${BUILD_ROOT:-"${src_dir}/build/k1-firmware"}
output_dir=${OUTPUT_DIR:-"${src_dir}/output"}
cross_compile=${CROSS_COMPILE:-riscv64-linux-gnu-}
jobs=${JOBS:-$(nproc)}
variant=${1:-all}

case "${variant}" in
standard|rva23|all)
	;;
*)
	echo "Usage: $0 [standard|rva23|all]" >&2
	exit 2
	;;
esac

configure_variant()
{
	local build_dir=$1
	local enable_emulation=$2
	local config_dir="${build_dir}/platform/generic/kconfig"
	local config_file="${config_dir}/.config"

	mkdir -p "${config_dir}"
	OPENSBI_SRC_DIR="${src_dir}" OPENSBI_PLATFORM=generic \
		OPENSBI_PLATFORM_SRC_DIR="${src_dir}/platform/generic" \
		KCONFIG_CONFIG="${config_file}" \
		python3 "${src_dir}/scripts/Kconfiglib/defconfig.py" \
		--kconfig "${src_dir}/Kconfig" \
		"${src_dir}/platform/generic/configs/defconfig"

	if [[ "${enable_emulation}" == yes ]]; then
		OPENSBI_SRC_DIR="${src_dir}" OPENSBI_PLATFORM=generic \
			OPENSBI_PLATFORM_SRC_DIR="${src_dir}/platform/generic" \
			KCONFIG_CONFIG="${config_file}" \
			python3 "${src_dir}/scripts/Kconfiglib/setconfig.py" \
			--kconfig "${src_dir}/Kconfig" SBI_ISA_EXT_EMU=y
	fi
}

build_variant()
{
	local name=$1
	local enable_emulation=$2
	local output_name=$3
	local build_dir="${build_root}/${name}"

	echo "==> Configuring K1 ${name} firmware"
	configure_variant "${build_dir}" "${enable_emulation}"

	echo "==> Building K1 ${name} firmware"
	make -C "${src_dir}" -j"${jobs}" \
		O="${build_dir}" \
		PLATFORM=generic \
		CROSS_COMPILE="${cross_compile}"

	mkdir -p "${output_dir}"
	install -m 0644 \
		"${build_dir}/platform/generic/firmware/fw_dynamic.itb" \
		"${output_dir}/${output_name}"
	echo "==> ${output_dir}/${output_name}"
}

if [[ "${variant}" == standard || "${variant}" == all ]]; then
	build_variant standard no fw_dynamic-k1.itb
fi

if [[ "${variant}" == rva23 || "${variant}" == all ]]; then
	build_variant rva23 yes fw_dynamic-k1-rva23.itb
fi
