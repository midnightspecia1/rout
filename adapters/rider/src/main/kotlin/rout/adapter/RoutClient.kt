package rout.adapter

import com.google.gson.JsonObject
import com.google.gson.JsonParser
import com.intellij.openapi.Disposable
import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.application.ModalityState
import com.intellij.openapi.components.Service
import com.intellij.openapi.components.service
import com.intellij.openapi.diagnostic.logger
import com.intellij.openapi.project.Project
import com.intellij.openapi.startup.ProjectActivity
import java.net.URI
import java.net.http.HttpClient
import java.net.http.WebSocket
import java.nio.file.Files
import java.nio.file.Path
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

private const val PORT = 47613
private const val RETRY_SECONDS = 3L

/** Starts the connection once per IDE process, whichever project opens first. */
class StartRoutClient : ProjectActivity {
    override suspend fun execute(project: Project) = service<RoutClient>().start()
}

/**
 * The WebSocket link to rout (Protocol v1, see PLAN.md). One per IDE
 * process: rout keys adapters by exe name, so a second connection would
 * just replace this one.
 *
 * rout may start after the IDE or restart with a new token, so the client
 * keeps reconnecting, re-reading the token file each time.
 */
@Service(Service.Level.APP)
class RoutClient : Disposable {
    private val log = logger<RoutClient>()
    private val http = HttpClient.newHttpClient()
    private val scheduler = Executors.newSingleThreadScheduledExecutor { r ->
        Thread(r, "rout-ipc").apply { isDaemon = true }
    }
    private val started = AtomicBoolean()
    @Volatile private var disposed = false

    // java.net.http allows one outstanding send per socket; chain them.
    private var sendChain: CompletableFuture<*> = CompletableFuture.completedFuture(null)

    fun start() {
        if (started.compareAndSet(false, true)) scheduler.execute(::connect)
    }

    override fun dispose() {
        disposed = true
        scheduler.shutdownNow()
    }

    private fun connect() {
        if (disposed) return
        val token = readToken()
        if (token == null) {
            retry()  // rout not running yet.
            return
        }
        http.newWebSocketBuilder()
            .buildAsync(URI.create("ws://127.0.0.1:$PORT"), Listener(token))
            .whenComplete { _, error -> if (error != null) retry() }
    }

    private fun retry() {
        if (!disposed) scheduler.schedule(::connect, RETRY_SECONDS, TimeUnit.SECONDS)
    }

    private fun readToken(): String? {
        val base = System.getenv("LOCALAPPDATA") ?: return null
        return runCatching { Files.readString(Path.of(base, "rout", "ipc-token")).trim() }
            .getOrNull()?.takeIf { it.isNotEmpty() }
    }

    private fun send(ws: WebSocket, msg: JsonObject) {
        val text = msg.toString()
        synchronized(this) {
            sendChain = sendChain.handle { _, _ -> null }.thenCompose { ws.sendText(text, true) }
        }
    }

    private fun onMessage(ws: WebSocket, text: String) {
        val msg = runCatching { JsonParser.parseString(text).asJsonObject }.getOrNull() ?: return
        val cmd = msg.get("cmd")?.takeIf { it.isJsonPrimitive }?.asString
        // Swing is only touched on the EDT; any modality, so hints also
        // work while a dialog is open.
        when (cmd) {
            "get_hints" -> {
                val req = msg.get("req")?.takeIf { it.isJsonPrimitive }?.asLong ?: return
                ApplicationManager.getApplication().invokeLater(
                    { send(ws, HintCollector.collectReply(req)) }, ModalityState.any())
            }
            "activate" -> {
                val id = msg.get("id")?.takeIf { it.isJsonPrimitive }?.asString ?: return
                ApplicationManager.getApplication().invokeLater({ HintCollector.activate(id) }, ModalityState.any())
                send(ws, JsonObject().apply { addProperty("type", "ack") })
            }
        }
    }

    private inner class Listener(private val token: String) : WebSocket.Listener {
        private val buffer = StringBuilder()

        override fun onOpen(ws: WebSocket) {
            val process = ProcessHandle.current()
            val exe = process.info().command().map { Path.of(it).fileName.toString().lowercase() }
                .orElse("rider64.exe")
            send(ws, JsonObject().apply {
                addProperty("type", "hello")
                addProperty("app", exe)
                addProperty("pid", process.pid())
                addProperty("token", token)
            })
            log.info("connected to rout as $exe")
            ws.request(1)
        }

        override fun onText(ws: WebSocket, data: CharSequence, last: Boolean): CompletionStage<*>? {
            buffer.append(data)
            if (last) {
                val text = buffer.toString()
                buffer.setLength(0)
                runCatching { onMessage(ws, text) }.onFailure { log.warn("bad message from rout", it) }
            }
            ws.request(1)
            return null
        }

        override fun onClose(ws: WebSocket, statusCode: Int, reason: String): CompletionStage<*>? {
            log.info("rout closed the connection: $statusCode $reason")
            retry()
            return null
        }

        override fun onError(ws: WebSocket, error: Throwable) {
            retry()
        }
    }
}
