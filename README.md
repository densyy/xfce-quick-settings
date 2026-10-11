# Quick Settings — Plugin para o Painel XFCE

Popup com controles de áudio, brilho, rede, energia, sessão e sistema.

## Instalar no Arch

```sh
sh install/arch/install.sh
```

## Instalar no Debian/Ubuntu

```sh
sh install/debian/install.sh
```

## Usar

Botão direito no painel → Adicionar Novos Itens → "Quick Settings".

## Pré-requisitos

- Compiladores: `gcc`, `make`, `pkg-config`
- Dev GTK/XFCE: `gtk3`, `xfce4-panel`, `libxfce4ui`, `sqlite`
- Ferramentas: `brightnessctl`, `upower`, `nmcli`, `bluetoothctl`, `pactl`, `xrandr`
- Painel XFCE em execução (`xfce4-panel`)
- Permissão sudo para instalar o plugin e a regra de backlight

## Desinstalar

```sh
sh install/arch/uninstall.sh
```

```sh
sh install/debian/uninstall.sh
```
