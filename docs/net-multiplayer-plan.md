# План: аркадный мультиплеер реального времени

> Ключевые факты о проекте и полный план реализации собраны в
> [`project-notes.md`](project-notes.md). Здесь — только состояние работ.

## Контекст

Задача: играть нескольким людям одновременно (до `HS_PLAYERS_MAX = 5`), без
пошаговости — семена не расставляются, заклинания запускаются в реальном
времени. Сначала LAN, позже интернет через relay.

## Архитектура

Сервер-авторитет со снапшотами, не lockstep:

* Хост — единственное место, где крутится симуляция; рассылает состояние.
* Клиент шлёт только ввод намерений, рисует удалённые мехосы по снапшотам.
* Удалённые мехосы на клиенте **не симулируются**.

Причина отказа от lockstep: `XRnd()` использует `static XRndValue`
(`xutil.cpp:358`) без сеттера, а `xm_random_generator` влияет на
`LCP_Solver` (`LCP_Solver.cpp:23`), то есть на физику, не на графику.
Повторить такой поток бит-в-бит на другой машине нельзя, а сломаться он
может молча.

## Три класса сущностей

| Класс | Что | Как синхронизируется |
|---|---|---|
| A | Мехосы игроков и ботов | Снапшот 20 Гц, интерполяция, ~50 Б на штуку |
| B | Баллистика: снаряды, бомбы, бонусы-метеориты | `MSG_SPAWN` / `MSG_DESPAWN`, клиент считает движение сам |
| C | Ходячие и самонаводящиеся: `BonusMan`, `Butterfly`, `Dragon`, `Shark`, `Whale`, `Frog`, `Worm`, зоны урона | `MSG_ACTOR_STATE` 10 Гц + interest management |
| D | Косметика: частицы, дым, звук | Не передаётся; `fxlabClientRND` с общим сидом |

Класс C нельзя ре-симулировать: `Butterfly::quant()` использует
`random(2000)` и `frnd(1.)` (`Butterfly.cpp:25-30`), `WickedButterfly`
наследует `AttackAndHomeObject` и самонаводится на игрока,
`BonusMan` прыгает в `setPosition(Vect3f(random(2048), ...))`
(`BonusMan.cpp:156`).

---

## Этап 1 — транспорт — СДЕЛАНО И ПРОВЕРЕНО

Файлы:

* `RUNTIME/net_socket.h` / `RUNTIME/net_socket.cpp` — изолированный сокетный
  слой. Единственное место в сборке с `<winsock2.h>`: windows.h конфликтует с
  `enum dbtype3ds { Unknown }` в `3dsftk.h` и `WritePrivateProfileString` в
  `XTool/port.h`, поэтому этот TU не включает проектных заголовков вообще.
* `RUNTIME/net_protocol.h` / `RUNTIME/net_protocol_types.h` — все форматы
  сообщений, квантование, константы.
* `RUNTIME/net_selftest.cpp` — самотест: хост и клиент в одном процессе,
  запускается флагом `-nettest`, пишет `nettest.log`, код возврата 0 при
  успехе.
* `RUNTIME/net_transport.h` / `RUNTIME/net_transport.cpp` — переписан целиком.

Что внутри транспорта:

* Заголовок отдельно от payload, лимит датаграммы 1200 байт.
* Фрагментация с reassembly по `frag_id`, отдельный буфер на пира.
* У каждого `netEndpoint` своя очередь надёжных сообщений; ACK снимает
  отправленные.
* Немедленный ACK на надёжный пакет, не только piggyback — иначе отправитель
  выжигает весь бюджет ретрансмитов до прихода подтверждения.
* Таймбаза `clocki()`, а не `global_time()`: последний замирает при
  `mchFreezeTime`.
* ACK обрабатывается даже для дубля — иначе потеря нашего ACK порождает
  бесконечный ретрансмит.
* Стабы для POSIX в том же файле.

Сверх плана: перегрузка `fxlabRndGenerator::Open(unsigned seed)` в
`FX/fxlabGeneral.h` + `.cpp` — понадобится на подэтапе 4d.

### Баги, найденные и исправленные по ходу самотеста

| Баг | Симптом | Исправление |
|---|---|---|
| `connect()` не ставил `hasAddr` | на тот же адрес создавался **второй** пир; два `sendSeq` бились с одним `recvSeq` и молча съедали пакеты | `ep -> hasAddr = 1` в `connect()` |
| `frag[NET_FRAG_PAYLOAD]` + 4 байта заголовка | переполнение стека на 4 байта при фрагментации | буфер `NET_MAX_PAYLOAD` |
| Буфер входящих `data[NET_MAX_PAYLOAD]` | собранное 4096-байтное сообщение било в 1190-байтный буфер, access violation | `data[NET_FRAG_MAX * NET_FRAG_PAYLOAD]` |
| `check_timeouts()` двойным шагом | каждый второй пир не проверялся на таймаут | один шаг |
| ACK только piggyback на keepalive | 14 ретрансмитов за 8 секунд на двух сообщениях | немедленный ACK-only пакет |

### Результат прогона (`bin/nettest.log`)

```
client got WELCOME from peer 1 (36 bytes)
host reassembled 4096 bytes, payload intact
client burst complete: 10000 datagrams
host   sent=11 recv=10015 retransmits=0 dropped=0
client sent=10015 recv=11 retransmits=0 dropped=0
burst sent 10000, received 10000, gaps 0
RESULT: PASS
```

---

## Этап 2 — сетевой ввод

* `struct netInput { tick; traction; rudder; buttons }` — уже в протоколе.
* В `Mechos` рядом с `virtual keyboard_control()` добавить
  `virtual net_control(const netInput&)` — тот же алгоритм, но вместо `XKey`
  и `XJoystick*` берёт значения из структуры.
* В `PointControlledObject::control()` добавить ветку для помеченных сетевых
  мехосов.
* Уставки `do_traction` / `do_rudder` передаются, а не результат фильтров:
  они чистая функция истории уставок и фиксированного `dt`.

## Этап 3 — сессия и лобби

* `netSession` уже существует, добавить состояния и обработчики:
  `MSG_HELLO` → `MSG_WELCOME`, `MSG_ROSTER`, `MSG_READY`, `MSG_START_GAME`.
* `start_tick` — абсолютный тик, к которому все приводят свой темп.
* Таймаут 30 с без пакета → `MSG_KICK`; освободившийся слот занимает бот.

## Этап 4 — снапшоты и акторы

* 4a. Снапшоты мехосов, `netRacerState`, 50 Б.
* 4b. `MSG_SPAWN` / `MSG_DESPAWN` для класса B. Передавать уже
  разыгранный `model_id`: `fxlabCreateBolide` считает
  `model + fxlabServerRND.RND(noise)` (`fxlabBodyInterface.h:11`) на хосте.
* 4c. `MSG_ACTOR_STATE` для класса C + interest management
  (`NET_ACTOR_VIEW_RADIUS = 1200`).
* 4d. Сидировать `fxlabClientRND` из `MSG_WELCOME`.

## Этап 5 — режим игры

* Флаг `mchNetGame` рядом с существующими в `mechosoma.h`.
* **Не** выставлять `mchPBEM_HiddenTime`: иначе `GameQuantRTO::Quant()`
  шорт-циркутит на `mechosoma.cpp:923` и до симуляции не дойдёт.
* Старт через `mchSetTimeMode(MCH_TIME_RUNNING)` (`mechosoma.cpp:828`).
* Боты на свободные слоты — `AI_Toggle()`.

## Этап 6 — UI

* Раскомментировать слот `OL_Y` с ID 4 в `ACS_MULTIPLAYER_SCR`
  (`ISCREEN.SCR:5245-5288`) во всех четырёх файлах.
* `aciMainMenuOption(ACI_STRING, ...)` по образцу `ACS_EXT.CPP:1722` для
  имени игрока и адреса сервера.
* События `ACS_EXT_CREATE_GAME` / `ACS_EXT_JOIN_GAME`.
* HUD: пинг и состав игроков через `mch_arcScrD`.

## Этап 7 — отдельный серверный бинарник

Новая цель `moonshine-dedicated`: тот же `GAME_SOURCES` без рендера, звука и
ввода — только тик, физика и сеть. Код сессии с этапов 3-4 уже не зависит от
`IGraph3d`.

## Этап 8 — интернет за NAT

STUN + собственный relay-сервер. После LAN-версии.

## Следующий шаг

Этап 2: хук ввода в `Mechos::control()` / `net_control()`.
