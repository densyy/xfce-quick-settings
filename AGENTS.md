# AGENTS.md — regras para agentes de IA neste repositório

## 1. Versionamento (obrigatório a cada mudança)

O plugin segue [Semantic Versioning](https://semver.org) simplificado,
no formato `MAJOR.MINOR.PATCH` (ex.: `1.1.0`).

Fonte única da verdade:

- `src/qs-core.h` → `#define QS_VERSION "x.y.z"`
- `install/common/desktop.sh` → `Version=x.y.z` (manter sincronizado)

**Toda vez que a IA alterar o código, a versão DEVE subir no mesmo
commit**, pela regra:

| Tipo de mudança | Exemplos | O que sobe |
|---|---|---|
| Funcionalidade nova, recurso novo, teste novo | nova seção, novo teste, label de versão | `MINOR`: `1.x.0` (zera o patch) |
| Correção, docs, refactor sem comportamento novo | bugfix, `SECURITY.md`, `AGENTS.md`, lint, seam de teste | `PATCH`: `1.0.x` |
| Quebra de compatibilidade | mudança de API/contrato | `MAJOR`: só com aval explícito do dono |

A versão aparece discreta na interface: tag `vX.Y.Z` no canto inferior
direito da página principal (classe CSS `qs-version`).

Checklist do commit:

1. Mudou `src/`, `tests/`, `install/` ou docs? Suba `QS_VERSION`.
2. Sincronize `install/common/desktop.sh`.
3. Teste `test-plugin.c` (`/plugin/version_format` e
   `/plugin/build_structure`) valida formato e exibição.

## 2. Padrões de código

- Sem tabs, sem comentários `//`, sem `g_print`, sem `TODO`.
- `git diff --check` limpo; `sh tests/test-lint.sh` verde.
- Nunca executar ferramentas reais de sistema nos testes: usar
  `tests/fakebin` (PATH prefixado) e `QS_FAKE_LOG` como prova.
- Sem display, testes de UI dão skip (`gtk_init_check`).

## 3. Commits

- 1 commit por tarefa, na branch de trabalho; sem push sem aval.
- Mensagens no formato `tipo(área): descrição curta`.
