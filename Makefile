APPNAME = main

CC      = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
SIZE    = arm-none-eabi-size

ARCH = -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard

# Rutas de CMSIS (core + device) y macro del microcontrolador
CORE_CMSIS   = /home/carlos/Comp-Arch-Emb-Sys/LAB5/cmsis_core/CMSIS/Core/Include
CMSIS_DEVICE = /home/carlos/Comp-Arch-Emb-Sys/LAB5/cmsis_device_f4/Include
CFLAGS  = -Wall -Wextra -std=c11 -ffreestanding -O2 -g $(ARCH)
CFLAGS += -DSTM32F411xE
CFLAGS += -Iinc -I$(CORE_CMSIS) -I$(CMSIS_DEVICE)
CFLAGS += -MMD -MP

LDFLAGS  = $(ARCH) -nostdlib -T linker_script.ld
LDFLAGS += -Wl,--no-relax,-Map=$(APPNAME).map
LDLIBS   = -lgcc

# Búsqueda automática de fuentes (excluyendo archivos .h)
C_SRC = $(wildcard *.c) $(wildcard src/*.c)
ASM_SRC = $(wildcard *.S) $(wildcard src/*.S)

# Mapeo directo a objetos (.o)
OBJS = $(C_SRC:.c=.o)
OBJS += $(ASM_SRC:.S=.o)
DEPS = $(OBJS:.o=.d)

STM_PROGRAMMER = "/mnt/c/Program Files/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI.exe"
all: $(APPNAME).elf $(APPNAME).bin

$(APPNAME).elf: $(OBJS) linker_script.ld
	$(CC) $(LDFLAGS) $(OBJS) $(LDLIBS) -o $@
	$(SIZE) -A -x $@

$(APPNAME).bin: $(APPNAME).elf
	$(OBJCOPY) -O binary $< $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f src/*.o src/*.d *.o *.d $(APPNAME).elf $(APPNAME).bin $(APPNAME).map

flash: $(APPNAME).bin
	$(STM_PROGRAMMER) -c port=usb1 -w $(APPNAME).bin 0x08000000 -v

-include $(DEPS)

.PHONY: all clean flash