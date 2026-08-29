# OBS VU Meter PRO

Plugin nativo para OBS Studio, com VU Meter estéreo em barras LED coloridas.

## Compilação Windows x64

O projeto foi preparado para ser compilado automaticamente pelo GitHub Actions em `windows-2022`, seguindo a arquitetura de build do template oficial de plugins do OBS.

1. Crie um repositório chamado `obs-vu-meter-pro` na conta `doctordj`.
2. Envie todos os arquivos deste projeto para a branch `main`.
3. Abra a aba **Actions**.
4. Selecione **Build OBS VU Meter PRO - Windows x64**.
5. Clique em **Run workflow**.
6. Ao terminar com **Success**, abra a execução e baixe o artefato `OBS-VU-Meter-PRO-Windows-x64`.

O ZIP gerado contém:

```text
obs-plugins/64bit/obs-vu-meter-pro.dll
data/obs-plugins/obs-vu-meter-pro/locale/en-US.ini
```

Compatibilidade alvo: Windows 11 x64 + OBS Studio 32.x.
