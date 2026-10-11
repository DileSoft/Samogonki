# Лобби-сервер (rendezvous) в Docker

Консольное приложение, которое сводит игроков: хост регистрирует матч, клиент
видит список и выбирает матч, сервер обменивает их публичными UDP-адресами, и
дальше машины играют напрямую. Игровой трафик через сервер **не** идёт.

Подробнее о протоколе и дизайне — в `docs/net-multiplayer-plan.md`, раздел
«Этап 8».

## Требования

* Docker (для сборки образа) — `docker --version`;
* Docker Compose v2 — по желанию, для `docker compose`.

Образ собирается **только из сервера**, не из игры: три `.cpp` и два заголовка,
никаких SDL, рендера и `3rdparty`.

## Сборка образа

Из **корня репозитория** (контекст должен видеть `LobbyServer/` и
`MechoSoma/RUNTIME/`):

```bash
docker build -f LobbyServer/Dockerfile -t moonshine-lobby .
```

## Запуск

```bash
docker run -d \
  --name moonshine-lobby \
  -p 8510:8510/tcp \
  -p 8510:8510/udp \
  --restart unless-stopped \
  moonshine-lobby
```

**Оба порта обязательны.** Управляющий канал — TCP, а зонд и знакомство — UDP,
на одном и том же номере. Если опубликовать только TCP, клиент подключится к
серверу и увидит список, но не получит адрес партнёра — это выглядит как
«сервер работает, а в игру не пускает».

### Логи

```bash
docker logs -f moonshine-lobby
```

В консоли виден список матчей и события: кто зарегистрировал матч, кто вошёл,
кто получил адрес партнёра.

### Другой порт

Порт задаётся аргументом `--port` (он же становится UDP-портом). Меняйте и
проброс, и аргумент согласованно:

```bash
docker run -d --name moonshine-lobby \
  -p 9000:9000/tcp -p 9000:9000/udp \
  moonshine-lobby --port 9000
```

## Запуск через Compose

```bash
cd LobbyServer
docker compose up -d --build     # собрать и поднять
docker compose logs -f           # смотреть
docker compose down              # остановить
```

Файл `LobbyServer/docker-compose.yml` уже пробрасывает оба порта.

## Самопроверка

Сервер умеет проверять себя сам: поднимает сервер и двух клиентов на loopback,
проходит регистрацию, список, вход и сверяет, что обе стороны получили
**настоящие** адреса друг друга.

```bash
docker run --rm moonshine-lobby --selftest
```

Ожидаемый хвост вывода: `RESULT: PASS`.

## Настройка клиентов

В игре: «Сетевая игра» → **Связь = Лобби**, в поле **«Лобби:»** — адрес сервера
(хост и клиент вводят один и тот же). Можно с портом: `203.0.113.5:8510`, или
`203.0.113.5` (порт по умолчанию 8510).

Хост: «Создать игру». Клиент: «Поиск» → выбрать матч в списке → дождаться, пока
хост нажмёт «Начать».

## Сеть и фаервол

* Открой на машине с Docker **и TCP, и UDP** на выбранном порту (8510). На VPS
  это чаще всего cloud-фаервол + `ufw`/`firewalld` **и** проброс в Docker.
* Если сервер за другим NAT — не важно, сервер лишь наблюдает публичный адрес
  игры; сам он должен быть доступен снаружи по обоим протоколам.
* Пробитие (hole punching) работает на большинстве NAT, но **симметричный NAT
  не пробьётся** — для него нужен relay, это следующий шаг. LAN и «плоские»
  NAT работают.

## Обновление после правок кода

```bash
git pull
docker build -f LobbyServer/Dockerfile -t moonshine-lobby .
docker rm -f moonshine-lobby
docker run -d --name moonshine-lobby \
  -p 8510:8510/tcp -p 8510:8510/udp --restart unless-stopped moonshine-lobby
```

## Ручная сборка без Docker

Тот же набор файлов, что и в образе:

```bash
g++ -std=c++17 -O2 -pthread \
    -ILobbyServer -IMechoSoma/RUNTIME \
    LobbyServer/main.cpp LobbyServer/lobby_server.cpp MechoSoma/RUNTIME/net_socket.cpp \
    -o moonshine-lobby
./moonshine-lobby --port 8510
```

Обычная сборка из CMake (цель `moonshine-lobby`, опция `BUILD_LOBBY_SERVER`)
собирает ровно то же самое.
