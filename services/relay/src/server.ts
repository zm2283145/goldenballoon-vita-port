import {createServer, type Server as HttpServer} from "node:http";
import {WebSocketServer, type RawData, type WebSocket} from "ws";
import {
  errorMessage,
  parseAdmissionRequest,
  RELAY_FRAME_HEADER_BYTES,
  RELAY_MAX_PAYLOAD_BYTES,
  RELAY_PROTOCOL_VERSION,
  RELAY_SUBPROTOCOL,
  type RelayErrorCode,
} from "./protocol.js";
import {RelayManager, type RelayManagerOptions} from "./relay.js";

export interface RelayServerOptions extends RelayManagerOptions {
  host?: string;
  port?: number;
  admissionTimeoutMs?: number;
  heartbeatMs?: number;
}

export interface RelayServerAddress {
  host: string;
  port: number;
}

export class RelayServer {
  readonly #options: Required<Pick<RelayServerOptions,
    "host" | "port" | "admissionTimeoutMs" | "heartbeatMs">>;
  readonly #manager: RelayManager;
  #http: HttpServer | null = null;
  #webSocket: WebSocketServer | null = null;
  #heartbeat: NodeJS.Timeout | null = null;

  constructor(options: RelayServerOptions = {}) {
    this.#options = {
      host: options.host ?? "0.0.0.0",
      port: options.port ?? 8080,
      admissionTimeoutMs: options.admissionTimeoutMs ?? 10_000,
      heartbeatMs: options.heartbeatMs ?? 15_000,
    };
    this.#manager = new RelayManager(options);
  }

  get roomCount(): number {
    return this.#manager.roomCount;
  }

  async start(): Promise<RelayServerAddress> {
    if (this.#http) throw new Error("relay server already started");
    const http = createServer((request, response) => {
      if (request.method === "GET" && request.url === "/healthz") {
        const body = JSON.stringify({
          status: "ok",
          protocolVersion: RELAY_PROTOCOL_VERSION,
          rooms: this.#manager.roomCount,
        });
        response.writeHead(200, {
          "content-type": "application/json",
          "cache-control": "no-store",
          "content-length": Buffer.byteLength(body),
        });
        response.end(body);
        return;
      }
      response.writeHead(404, {
        "content-type": "text/plain; charset=utf-8",
        "cache-control": "no-store",
      });
      response.end("not found\n");
    });
    const webSocket = new WebSocketServer({
      noServer: true,
      maxPayload: RELAY_FRAME_HEADER_BYTES + RELAY_MAX_PAYLOAD_BYTES,
      perMessageDeflate: false,
      handleProtocols: protocols =>
        protocols.has(RELAY_SUBPROTOCOL) ? RELAY_SUBPROTOCOL : false,
    });
    http.on("upgrade", (request, socket, head) => {
      if (request.url !== "/relay" ||
          request.headers["sec-websocket-protocol"]?.split(",")
            .map(value => value.trim())
            .includes(RELAY_SUBPROTOCOL) !== true) {
        socket.write("HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n");
        socket.destroy();
        return;
      }
      webSocket.handleUpgrade(request, socket, head, client => {
        webSocket.emit("connection", client, request);
      });
    });
    webSocket.on("connection", socket => this.#accept(socket));
    await new Promise<void>((resolve, reject) => {
      const onError = (error: Error): void => {
        http.off("listening", onListening);
        reject(error);
      };
      const onListening = (): void => {
        http.off("error", onError);
        resolve();
      };
      http.once("error", onError);
      http.once("listening", onListening);
      http.listen(this.#options.port, this.#options.host);
    });
    this.#http = http;
    this.#webSocket = webSocket;
    this.#heartbeat = setInterval(
      () => this.#manager.heartbeat(), this.#options.heartbeatMs);
    this.#heartbeat.unref();
    const address = http.address();
    if (!address || typeof address === "string") {
      await this.stop();
      throw new Error("relay server did not bind a TCP address");
    }
    return {host: this.#options.host, port: address.port};
  }

  async stop(): Promise<void> {
    if (this.#heartbeat) {
      clearInterval(this.#heartbeat);
      this.#heartbeat = null;
    }
    this.#manager.close();
    const webSocket = this.#webSocket;
    this.#webSocket = null;
    if (webSocket) {
      for (const client of webSocket.clients) client.terminate();
      webSocket.close();
    }
    const http = this.#http;
    this.#http = null;
    if (http) {
      await new Promise<void>(resolve => http.close(() => resolve()));
    }
  }

  #accept(socket: WebSocket): void {
    let state: "pending" | "browser" | "room" = "pending";
    const timeout = setTimeout(() => {
      if (state === "pending") this.#reject(socket, "protocol");
    }, this.#options.admissionTimeoutMs);
    timeout.unref();

    socket.on("pong", () => this.#manager.markAlive(socket));
    socket.on("close", () => {
      clearTimeout(timeout);
      this.#manager.remove(socket);
    });
    socket.on("error", () => {
      clearTimeout(timeout);
      this.#manager.remove(socket);
    });
    socket.on("message", (data: RawData, isBinary: boolean) => {
      if (state !== "room") {
        if (isBinary) {
          this.#reject(socket, "protocol");
          return;
        }
        const request = parseAdmissionRequest(data.toString());
        if (!request) {
          this.#reject(socket, "protocol");
          return;
        }
        const error = this.#manager.admit(socket, request);
        if (error) {
          if (state === "browser") {
            if (socket.readyState === socket.OPEN) {
              socket.send(errorMessage(error));
            }
          } else {
            this.#reject(socket, error);
          }
          return;
        }
        state = request.type === "browse" ? "browser" : "room";
        clearTimeout(timeout);
        return;
      }
      if (!isBinary) {
        this.#reject(socket, "protocol");
        return;
      }
      let bytes: Buffer;
      if (Buffer.isBuffer(data)) bytes = data;
      else if (Array.isArray(data)) bytes = Buffer.concat(data);
      else bytes = Buffer.from(data);
      const error = this.#manager.receiveBinary(socket, bytes);
      if (error) this.#reject(socket, error);
    });
  }

  #reject(socket: WebSocket, code: RelayErrorCode): void {
    if (socket.readyState === socket.OPEN) {
      socket.send(errorMessage(code));
      socket.close(code === "rate_limited" ? 4008 : 4002, code);
    } else {
      socket.terminate();
    }
  }
}
