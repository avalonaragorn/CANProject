#/bin/sh 

./qemu-3.0.0/arm-softmmu/qemu-system-arm -kernel qemu-rpi-kernel/kernel-qemu-4.14.50-stretch \
        	-cpu arm1176 -m 256 -M versatilepb \
		-dtb qemu-rpi-kernel/versatile-pb.dtb \
		-no-reboot -append "root=/dev/sda2 panic=1 rootfstype=ext4 rw" \
		-net nic -net user,hostfwd=tcp::5022-:22 \
		-hda pi_1_bak_1.img
		
		#-hda raspberry_pi_1.img
