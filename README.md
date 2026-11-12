# OBS VU Meter PRO

Plugin nativo para OBS Studio, com VU Meter estéreo em barras LED coloridas, layouts gráficos e opções de fundo/chroma key.

## OBS Studio 33+ (Windows x64)

A distribuição OBS 33+ usa a estrutura de plugin atualizada:

```
C:\ProgramData\obs-studio\plugins\obs-vu-meter-pro\
  obs-vu-meter-pro.dll
  data\locale\en-US.ini
  data\backgrounds\...
```

O instalador `OBS-VU-Meter-PRO-OBS33-Setup.exe` instala o plugin nessa estrutura e remove cópias anteriores gerenciadas nas localizações legacy conhecidas. Feche o OBS Studio antes de instalar e reabra-o após a instalação.

## Compilação

O GitHub Actions workflow **Build OBS VU Meter PRO - Windows OBS 33** constrói o plugin com o template oficial de plugins do OBS, monta o layout OBS 33+ e publica dois artefatos na execução:

- `OBS-VU-Meter-PRO-OBS33-Setup.exe` — instalador Windows x64
- `OBS-VU-Meter-PRO-OBS33-Windows-x64.zip` — instalação manual

O ZIP contém o DLL na raiz da pasta do plugin e os arquivos de dados dentro de `data/`.
