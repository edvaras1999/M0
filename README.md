# NahIDWinCore — M0 (sonda de golpes)

Skyrim **LE 1.9.32 / SKSE 1.7.3**. Este marco **não altera o jogo**: só escreve um log com os dados de cada golpe,
para confirmarmos o que cada campo do `HitData` contém antes de construir o bloqueio/parry/poise (sem Inpa) por cima.

## Como usar
1. Crie um repositório novo no GitHub e envie estas pastas (`.github`, `core`, `dist`, `README.md`).
2. Aba **Actions → Build NahIDWinCore → Run workflow**; no fim baixe o artefacto `NahIDWinCore-dll`.
3. Instale com o MO2 (ou à mão): `NahIDWinCore.dll` e `NahIDWinCore.ini` em `Data\SKSE\Plugins\`.
   Pode correr junto do NahIDWin (o hook do M0 só lê; mas para o teste mais limpo, desligue o NahIDWin).
4. Jogue 5–10 minutos: bloqueie golpes, faça parry, leve golpes sem bloquear, bata em NPCs, use arco.
5. Envie `Documentos\My Games\Skyrim\SKSE\NahIDWinCore.log`.

## INI
`Enable` (0 desliga tudo), `MaxLogs` (padrão 300), `OnlyPlayerInvolved` (1 = só golpes com o jogador).

## Segurança
O hook confirma os bytes de `0x006E4770` antes de instalar; se não baterem, não instala e escreve o motivo no log.
Leituras de memória são protegidas e a função original é sempre chamada.
