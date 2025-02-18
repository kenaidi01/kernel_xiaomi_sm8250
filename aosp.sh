#!/bin/bash

if [[ $(git remote | grep ksu) != "ksu" ]]; then
     git remote add ksu https://github.com/tiann/KernelSU
     git fetch ksu main > /dev/null 2>&1 
fi

KSUVER=$(git rev-list --count ksu/main)

DEVICE=$1
KERNEL_DIR=$(dirname $(realpath ${BASH_SOURCE[0]}))
DTSI_DIR=$KERNEL_DIR/arch/arm64/boot/dts/vendor/qcom
APOLLO=$DTSI_DIR/dsi-panel-j3s-37-02-0a-dsc-video.dtsi
ALIOTH=$DTSI_DIR/dsi-panel-k11a-38-08-0a-dsc-cmd.dtsi
MUNCH=$DTSI_DIR/dsi-panel-l11r-38-08-0a-dsc-cmd.dtsi

aosp_build() {
	sed -i 's/qcom,mdss-pan-physical-width-dimension = <695>;/qcom,mdss-pan-physical-width-dimension = <70>;/g' $APOLLO
	sed -i 's/qcom,mdss-pan-physical-height-dimension = <1544>;/qcom,mdss-pan-physical-height-dimension = <155>;/g' $APOLLO
	sed -i 's/qcom,mdss-pan-physical-width-dimension = <695>;/qcom,mdss-pan-physical-width-dimension = <70>;/g' $ALIOTH
	sed -i 's/qcom,mdss-pan-physical-height-dimension = <1546>;/qcom,mdss-pan-physical-height-dimension = <155>;/g' $ALIOTH
	sed -i 's/qcom,mdss-pan-physical-width-dimension = <695>;/qcom,mdss-pan-physical-width-dimension = <70>;/g' $MUNCH
	sed -i 's/qcom,mdss-pan-physical-height-dimension = <1546>;/qcom,mdss-pan-physical-height-dimension = <155>;/g' $MUNCH
	sed -i "s/CONFIG_OPLUS=y/# CONFIG_OPLUS is not set/g" $KERNEL_DIR/arch/arm64/configs/vendor/${DEVICE}_defconfig
	sed -i "s/CONFIG_OPLUS_FEATURE_EROFS=y/# CONFIG_OPLUS_FEATURE_EROFS is not set/g" $KERNEL_DIR/arch/arm64/configs/vendor/${DEVICE}_defconfig
}

build_ksu() {
	sed -i "s/CONFIG_GIT_VERSION=0/CONFIG_GIT_VERSION=${KSUVER}/g" $KERNEL_DIR/arch/arm64/configs/vendor/${DEVICE}_defconfig
	sed -i 's/# CONFIG_KSU is not set/CONFIG_KSU=y/g' $KERNEL_DIR/arch/arm64/configs/vendor/${DEVICE}_defconfig
	sed -i 's/# CONFIG_OVERLAY_FS_REDIRECT_DIR is not set/CONFIG_OVERLAY_FS_REDIRECT_DIR=y/g' $KERNEL_DIR/arch/arm64/configs/vendor/${DEVICE}_defconfig
	sed -i 's/# CONFIG_OVERLAY_FS_INDEX is not set/CONFIG_OVERLAY_FS_INDEX=y/g' $KERNEL_DIR/arch/arm64/configs/vendor/${DEVICE}_defconfig
	
	sed -i 's/# CONFIG_KSU_SUSFS is not set/CONFIG_KSU_SUSFS=y/g' $KERNEL_DIR/arch/arm64/configs/vendor/${DEVICE}_defconfig
	sed -i 's/# CONFIG_KSU_SUSFS_SUS_PATH is not set/CONFIG_KSU_SUSFS_SUS_PATH=y/g' $KERNEL_DIR/arch/arm64/configs/vendor/${DEVICE}_defconfig
	sed -i 's/# CONFIG_KSU_SUSFS_SUS_MOUNT is not set/CONFIG_KSU_SUSFS_SUS_MOUNT=y/g' $KERNEL_DIR/arch/arm64/configs/vendor/${DEVICE}_defconfig
	sed -i 's/# CONFIG_KSU_SUSFS_SUS_KSTAT is not set/CONFIG_KSU_SUSFS_SUS_KSTAT=y/g' $KERNEL_DIR/arch/arm64/configs/vendor/${DEVICE}_defconfig
	sed -i 's/# CONFIG_KSU_SUSFS_SUS_OVERLAYFS is not set/CONFIG_KSU_SUSFS_SUS_OVERLAYFS=y/g' $KERNEL_DIR/arch/arm64/configs/vendor/${DEVICE}_defconfig
	sed -i 's/# CONFIG_KSU_SUSFS_TRY_UMOUNT is not set/CONFIG_KSU_SUSFS_TRY_UMOUNT=y/g' $KERNEL_DIR/arch/arm64/configs/vendor/${DEVICE}_defconfig
	sed -i 's/# CONFIG_KSU_SUSFS_SPOOF_UNAME is not set/CONFIG_KSU_SUSFS_SPOOF_UNAME=y/g' $KERNEL_DIR/arch/arm64/configs/vendor/${DEVICE}_defconfig
	sed -i 's/# CONFIG_KSU_SUSFS_ENABLE_LOG is not set/CONFIG_KSU_SUSFS_ENABLE_LOG=y/g' $KERNEL_DIR/arch/arm64/configs/vendor/${DEVICE}_defconfig
	sed -i 's/# CONFIG_KSU_SUSFS_HIDE_KSU_SUSFS_SYMBOLS is not set/CONFIG_KSU_SUSFS_HIDE_KSU_SUSFS_SYMBOLS=y/g' $KERNEL_DIR/arch/arm64/configs/vendor/${DEVICE}_defconfig
	sed -i 's/# CONFIG_KSU_SUSFS_SPOOF_PROC_CMDLINE is not set/CONFIG_KSU_SUSFS_SPOOF_PROC_CMDLINE=y/g' $KERNEL_DIR/arch/arm64/configs/vendor/${DEVICE}_defconfig
	sed -i 's/# CONFIG_OVERLAY_FS_REDIRECT_DIR is not set/CONFIG_OVERLAY_FS_REDIRECT_DIR=y/g' $KERNEL_DIR/arch/arm64/configs/vendor/${DEVICE}_defconfig
	sed -i 's/# CONFIG_KSU_SUSFS_OPEN_REDIRECT is not set/CONFIG_KSU_SUSFS_OPEN_REDIRECT=y/g' $KERNEL_DIR/arch/arm64/configs/vendor/${DEVICE}_defconfig
}


aosp_build
build_ksu
