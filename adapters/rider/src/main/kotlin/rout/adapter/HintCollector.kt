package rout.adapter

import com.google.gson.JsonArray
import com.google.gson.JsonObject
import com.intellij.openapi.actionSystem.impl.ActionButton
import com.intellij.openapi.wm.IdeFocusManager
import com.intellij.ui.HyperlinkLabel
import com.intellij.ui.tabs.impl.TabLabel
import java.awt.Component
import java.awt.Container
import java.awt.KeyboardFocusManager
import java.awt.Rectangle
import java.lang.ref.WeakReference
import javax.swing.AbstractButton
import javax.swing.JComboBox
import javax.swing.JComponent
import javax.swing.SwingUtilities
import javax.swing.text.JTextComponent
import kotlin.math.roundToInt

private const val MAX_HINTS = 676  // Same cap as Core: 26 * 26 labels.
private const val MIN_SIDE = 6

/**
 * Finds the widgets of the active IDE window worth a hint. Runs on the
 * EDT. Most are clicked by Core itself ("click": true); text components
 * are focused here instead, since a click in the middle of an editor
 * would move the caret.
 */
object HintCollector {
    private enum class Kind { Click, Focus }
    private class Hint(val component: Component, val rect: Rectangle, val name: String, val kind: Kind)

    // From the last get_hints; ids are indexes into it.
    private var last: List<WeakReference<Component>> = emptyList()

    fun collectReply(req: Long): JsonObject {
        val hints = collect()
        last = hints.map { WeakReference(it.component) }
        val items = JsonArray()
        hints.forEachIndexed { i, h ->
            items.add(JsonObject().apply {
                addProperty("id", i.toString())
                add("rect", JsonArray().apply {
                    add(h.rect.x); add(h.rect.y); add(h.rect.width); add(h.rect.height)
                })
                if (h.name.isNotEmpty()) addProperty("name", h.name)
                if (h.kind == Kind.Click) addProperty("click", true)
            })
        }
        return JsonObject().apply {
            addProperty("type", "hints")
            addProperty("req", req)
            add("items", items)
        }
    }

    fun activate(id: String) {
        val component = id.toIntOrNull()?.let { last.getOrNull(it)?.get() } ?: return
        if (!component.isShowing) return
        IdeFocusManager.getGlobalInstance().requestFocus(component, true)
    }

    private fun collect(): List<Hint> {
        val window = KeyboardFocusManager.getCurrentKeyboardFocusManager().activeWindow ?: return emptyList()
        val out = ArrayList<Hint>()
        visit(window, out)
        // Popups and tool windows floating above the frame.
        for (owned in window.ownedWindows) if (owned.isShowing) visit(owned, out)
        return out
    }

    private fun visit(c: Component, out: MutableList<Hint>) {
        if (out.size >= MAX_HINTS || !c.isShowing) return
        val kind = kindOf(c)
        if (kind != null && c.isEnabled) {
            screenRect(c)?.let { out += Hint(c, it, nameOf(c), kind) }
            // A widget's insides (icon, label, arrow) are not targets.
            if (c !is TabLabel) return
        }
        if (c is Container) for (child in c.components) visit(child, out)
    }

    private fun kindOf(c: Component): Kind? = when {
        c is JTextComponent -> if (c.isEditable && c.isFocusable) Kind.Focus else null
        c.parent is JComboBox<*> -> null  // The combo box's own arrow button.
        c is ActionButton || c is AbstractButton || c is JComboBox<*> || c is TabLabel || c is HyperlinkLabel ->
            Kind.Click
        else -> null
    }

    private fun nameOf(c: Component): String {
        val text = when (c) {
            is AbstractButton -> c.text
            is ActionButton -> c.action.templatePresentation.text
            else -> null
        }
        return (text ?: c.accessibleContext?.accessibleName ?: (c as? JComponent)?.toolTipText ?: "").trim()
    }

    /** Visible part of the component in screen physical pixels, or null. */
    private fun screenRect(c: Component): Rectangle? {
        val visible = (c as? JComponent)?.visibleRect ?: Rectangle(0, 0, c.width, c.height)
        if (visible.width < MIN_SIDE || visible.height < MIN_SIDE) return null
        val origin = visible.location
        SwingUtilities.convertPointToScreen(origin, c)
        return toPhysical(Rectangle(origin.x, origin.y, visible.width, visible.height), c)
    }

    // Java works in scaled user space. On Windows the JBR keeps each
    // screen's origin in device pixels and scales only within the screen.
    private fun toPhysical(r: Rectangle, c: Component): Rectangle {
        val gc = c.graphicsConfiguration ?: return r
        val sx = gc.defaultTransform.scaleX
        val sy = gc.defaultTransform.scaleY
        val screen = gc.bounds
        return Rectangle(
            (screen.x + (r.x - screen.x) * sx).roundToInt(),
            (screen.y + (r.y - screen.y) * sy).roundToInt(),
            (r.width * sx).roundToInt(),
            (r.height * sy).roundToInt(),
        )
    }
}
