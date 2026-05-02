build_arch	= arm64
defconfig	= defconfig
flavours	= asahi-arm
build_image    = vmlinuz.efi
kernel_file    = arch/$(build_arch)/boot/vmlinuz.efi
install_file	= vmlinuz
no_dumpfile = true
uefi_signed = false

vdso		= vdso_install

do_tools_usbip  = true
do_tools_cpupower = true
do_tools_perf   = true
do_tools_perf_jvmti = true
do_tools_perf_python = true
do_tools_bpftool = true
do_tools_rtla = true

do_dtbs		= true

rustc = /usr/lib/rust-1.89/bin/rustc
rustfmt = /usr/lib/rust-1.89/bin/rustfmt
bindgen = /lib/bindgen-0.71/bin/bindgen
