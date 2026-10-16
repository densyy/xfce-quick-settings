# Quick Settings — alvos de desenvolvimento e validação.
# analyze: análise estática profunda (GCC -fanalyzer) com as flags do build oficial.
# cppcheck: opcional, roda se estiver no PATH.
# smoke: build + verificação de dlopen + roteiro manual.
# test: suite GTest (tests/) + regressão estrutural.
# lint: cppcheck + fanalyzer + clang-tidy + grep-gates (falha se achar algo).
# build-matrix: -O0/-O2/-Os × cc/gcc/clang com -Werror + checksec/nm/reprodutível.
# spawn-budget: teto de processos spawnados pelo fluxo wifi (via QS_FAKE_LOG).
# sanitizers: suite inteira sob ASan+LSan+UBSan (sem valgrind nesta máquina).
# CI futuro sugerido: job com -fsanitize=address,undefined -fno-sanitize-recover=all.

PKGS = gtk+-3.0 libxfce4panel-2.0 libxfce4ui-2 libxfce4util-1.0 sqlite3
CFLAGS_QS = -Wall -Wextra -Wconversion -g -std=gnu11 -fPIC -O2 -shared -D_FORTIFY_SOURCE=3
SRCS = $(wildcard src/*.c)

.PHONY: build analyze cppcheck smoke test lint build-matrix spawn-budget sanitizers

build:
	sh install/common/build.sh

analyze:
	gcc -fanalyzer $(CFLAGS_QS) -o /tmp/quicksettings-analyze.so $(SRCS) \
	  $(shell pkg-config --cflags $(PKGS)) $(shell pkg-config --libs $(PKGS)) -lsqlite3

cppcheck:
	@command -v cppcheck >/dev/null 2>&1 && \
	  cppcheck --enable=warning,performance,portability --std=c11 -q src/ || \
	  echo "cppcheck não encontrado no PATH — alvo pulado"

smoke: build
	sh scripts/smoke.sh

test: build
	sh tests/run-tests.sh && sh tests/test-regress.sh

lint:
	sh tests/test-lint.sh

build-matrix:
	sh tests/test-build.sh

spawn-budget:
	sh tests/test-spawn-budget.sh

sanitizers:
	sh tests/run-sanitizers.sh
