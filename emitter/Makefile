CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra
# 槽位完整性 C1(a)：Particle 全字段初始化少一个就按错误处理。
# particle_slots.h 内部也用 pragma 自带了同样的 error，直接编 .c 也生效。
CFLAGS += -Wmissing-field-initializers -Werror=missing-field-initializers

NEG_DIR := build/compile-gate

all: emitter

emitter: src/emitter.c src/particle_slots.h
	$(CC) $(CFLAGS) $< -o $@ -lm

# 编译期卡口负样本：复制一份头文件，在清单展开之外私塞一个未登记字段，
# 编译器必须拒绝且错误信息要点出该字段名；放行了就是卡口失效。
test-compile-gate:
	@rm -rf $(NEG_DIR) && mkdir -p $(NEG_DIR)
	@sed 's|PARTICLE_FIELDS(PARTICLE_STRUCT_DECL)|PARTICLE_FIELDS(PARTICLE_STRUCT_DECL) unsigned smuggled_unlisted_field;|' \
		src/particle_slots.h > $(NEG_DIR)/particle_slots.h
	@printf '#include "particle_slots.h"\nint main(void){ Particle p; (void)p; return (int)sizeof(p); }\n' \
		> $(NEG_DIR)/neg.c
	@if $(CC) $(CFLAGS) -I$(NEG_DIR) -c $(NEG_DIR)/neg.c -o $(NEG_DIR)/neg.o \
			2> $(NEG_DIR)/err.txt; then \
		echo "compile_gate_rejects_unlisted_field=0"; \
		echo "FATAL: 清单外字段 smuggled_unlisted_field 竟然编译通过"; exit 1; \
	else \
		if grep -q "smuggled_unlisted_field" $(NEG_DIR)/err.txt; then \
			echo "compile_gate_rejects_unlisted_field=1"; \
		else \
			echo "compile_gate_rejects_unlisted_field=0"; cat $(NEG_DIR)/err.txt; exit 1; \
		fi; \
	fi

test: emitter test-compile-gate
	./emitter

clean:
	rm -f emitter
	rm -rf build

.PHONY: all test clean test-compile-gate
