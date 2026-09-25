# HTTP/1.1 Calculator

A small HTTP/1.1 calculator server and client written in C with POSIX sockets. The server keeps each TCP connection open and processes requests sequentially.

## Build

```sh
cc -std=c11 -Wall -Wextra -O2 -o bserve bserve.c
cc -std=c11 -Wall -Wextra -O2 -o bcurl bcurl.c
```

## Run

Start the server:

```sh
./bserve ./www 9000
```

Use the client. Multiple targets use one TCP connection:

```sh
./bcurl -v localhost:9000 /add?a=2&b=3 /sub?a=10&b=4 /mul?a=6&b=7 /div?a=9&b=3
```

A valid response has status `200`, `Content-Type: text/plain`, and an exact `Content-Length`.

## Routes

| Request | Result |
| --- | ---: |
| `GET /add?a=2&b=3` | `200`, body `5` |
| `GET /sub?a=10&b=4` | `200`, body `6` |
| `GET /mul?a=6&b=7` | `200`, body `42` |
| `GET /div?a=9&b=3` | `200`, body `3` |
| `GET /div?a=1&b=0` | `400` |
| `GET /add?a=x&b=3` | `400` |
| `GET /pow?a=2&b=8` | `404` |
| `POST /add?a=2&b=3` | `405` |

HTTP/1.1 requests must include a non-empty `Host` header. Missing or malformed headers return `400`. The server supports `Connection: close` and otherwise keeps the connection alive.

## Files

- `bserve.c`: persistent HTTP/1.1 calculator server
- `bcurl.c`: HTTP/1.1 client using one connection for all targets
- `hexdump.txt`: annotated bytes for a complete HTTP request and response
