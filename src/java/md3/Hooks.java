package md3;

import java.lang.reflect.*;
import java.util.*;
import java.io.*;

/** Narrow, fail-open SystemUI hooks. No global theme flags or ROM files changed. */
public final class Hooks {
    public static native Method hook(Member target, Object hooker, Method callback);
    public static native boolean deopt(Member target);
    private static final List<Callback> KEEP = new ArrayList<>();
    private static boolean installed;
    private static int failures;
    private static final Map<Object, Surface> SURFACES = new WeakHashMap<>();
    private static final Set<String> LOGGED = new HashSet<>();
    private static Class<?> seekClass;
    private static Field baseDrawable, mixDrawing, progressRadius, inactiveDrawable, backgroundRect;
    private static Method drawableColor, drawableRadius, drawableBounds;
    private static Constructor<?> drawableCtor;
    private static File statusFile;
    private static Field scrimName, scrimAlpha;
    private static Object tintPaint;
    private static Method paintColor, canvasRect;

    private Hooks() {}
    public static void log(String message) {
        try {
            Class.forName("android.util.Log").getMethod("i", String.class, String.class)
                    .invoke(null, "MD3Runtime", message);
        } catch (Throwable ignored) {}
        if (statusFile != null) {
            try (FileWriter writer = new FileWriter(statusFile, true)) {
                writer.write(message + "\n");
            } catch (Throwable ignored) {}
        }
    }
    private static void fail(Throwable error) {
        if (failures++ < 8) log("fail-open: " + error);
    }
    private static Field field(Class<?> cls, String name) throws Exception {
        while (cls != null) {
            try {
                Field f = cls.getDeclaredField(name);
                f.setAccessible(true);
                return f;
            } catch (NoSuchFieldException ignored) { cls = cls.getSuperclass(); }
        }
        throw new NoSuchFieldException(name);
    }
    private static Object call(Object receiver, String name, Class<?>[] types, Object... args)
            throws Exception {
        Method method = receiver.getClass().getMethod(name, types);
        method.setAccessible(true);
        return method.invoke(receiver, args);
    }
    private static Object call(Object receiver, String name) throws Exception {
        return call(receiver, name, new Class<?>[0]);
    }
    private static boolean night(Object context) throws Exception {
        Object resources = call(context, "getResources");
        Object configuration = call(resources, "getConfiguration");
        return (configuration.getClass().getField("uiMode").getInt(configuration) & 0x30) == 0x20;
    }
    private static int token(Object context, String name) throws Exception {
        Object resources = call(context, "getResources");
        int id = (Integer) call(resources, "getIdentifier",
                new Class<?>[]{String.class, String.class, String.class}, name, "color", "android");
        if (id == 0) throw new IllegalArgumentException("Missing Monet token: " + name);
        return (Integer) call(context, "getColor", new Class<?>[]{int.class}, id);
    }
    private static Callback install(Method target, int action) throws Exception {
        target.setAccessible(true);
        Callback cb = new Callback(target, action);
        Method backup = hook(target, cb, Callback.class.getMethod("call", Object[].class));
        if (backup == null) throw new IllegalStateException("LSPlant Hook failed: " + target);
        backup.setAccessible(true);
        cb.backup = backup;
        KEEP.add(cb);
        log("hooked " + target.getDeclaringClass().getName() + "." + target.getName());
        return cb;
    }
    public static void boot() {
        try {
            Class<?> context = Class.forName("android.content.Context");
            install(Class.forName("android.app.Application").getDeclaredMethod("attach", context), 0);
            // attach is small and may be AOT-inlined in the framework's factory callers.
            for (String name : new String[]{"android.app.LoadedApk", "android.app.Instrumentation"}) {
                for (Method method : Class.forName(name).getDeclaredMethods()) {
                    if (method.getName().startsWith("makeApplication")
                            || method.getName().equals("newApplication")) {
                        method.setAccessible(true);
                        log("deopt app factory " + method.getName() + "=" + deopt(method));
                    }
                }
            }
        } catch (Throwable error) { fail(error); }
    }
    /** Called by the delayed native worker after the OEM post-fork runtime finishes. */
    public static void bootLate() {
        try {
            final Object app = Class.forName("android.app.ActivityThread")
                    .getMethod("currentApplication").invoke(null);
            if (app == null) throw new IllegalStateException("late boot without application");
            Class<?> looper = Class.forName("android.os.Looper");
            Object main = looper.getMethod("getMainLooper").invoke(null);
            Class<?> handler = Class.forName("android.os.Handler");
            Object h = handler.getConstructor(looper).newInstance(main);
            handler.getMethod("post", Runnable.class).invoke(h, new Runnable() {
                public void run() {
                    log("late setup on SystemUI main thread after OEM runtime");
                    setup(app);
                }
            });
        } catch (Throwable error) { fail(error); }
    }
    private static synchronized void setup(Object context) {
        if (installed) return;
        installed = true;
        try {
            statusFile = new File((File)call(context, "getFilesDir"), "md3-runtime-status.txt");
            try (FileWriter writer = new FileWriter(statusFile)) {
                writer.write("pid=" + Class.forName("android.os.Process").getMethod("myPid").invoke(null) + "\n");
            }
        } catch (Throwable ignored) {}
        try {
            ClassLoader loader = (ClassLoader) call(context, "getClassLoader");
            // Keep all stock light/dark blur recipes and tile blending untouched.
            // Apply only the final dark shade overlay, independent of OEM blur path.
            Class<?> view = Class.forName("com.android.systemui.scrim.ScrimView", false, loader);
            scrimName = field(view, "mScrimName");
            scrimAlpha = field(view, "mViewAlpha");
            Class<?> paint = Class.forName("android.graphics.Paint");
            tintPaint = paint.getConstructor().newInstance();
            paintColor = paint.getMethod("setColor", int.class);
            canvasRect = Class.forName("android.graphics.Canvas").getMethod("drawRect",
                    float.class, float.class, float.class, float.class, paint);
            install(view.getDeclaredMethod("onDraw", Class.forName("android.graphics.Canvas")), 3);
        } catch (Throwable error) { fail(error); }
        try {
            ClassLoader loader = (ClassLoader) call(context, "getClassLoader");
            seekClass = Class.forName("com.oplus.systemui.qs.base.seek.OplusQsVerticalSeekBar", true, loader);
            baseDrawable = field(seekClass, "baseMixColorDrawable");
            inactiveDrawable = field(seekClass, "mInactiveTrackDrawable");
            backgroundRect = field(seekClass, "mBackgroundRect");
            mixDrawing = field(seekClass, "isDrawingWithMixColor");
            progressRadius = field(seekClass, "mCurProgressRadius");
            Class<?> gd = Class.forName("android.graphics.drawable.GradientDrawable");
            drawableCtor = gd.getConstructor();
            drawableColor = gd.getMethod("setColor", int.class);
            drawableRadius = gd.getMethod("setCornerRadius", float.class);
            drawableBounds = gd.getMethod("setBounds", Class.forName("android.graphics.Rect"));
            install(seekClass.getDeclaredMethod("onDraw", Class.forName("android.graphics.Canvas")), 2);
            log("ready: final dark scrim tint; solid neutral1_900/50 inactive tracks; active path unchanged");
        } catch (Throwable error) { fail(error); }
    }
    private static void tint(Object view, Object canvas) throws Exception {
        if (!"behind_scrim".equals(scrimName.get(view))) return;
        Object context = call(view, "getContext");
        if (!night(context)) return;
        float visibility = scrimAlpha.getFloat(view);
        if (visibility <= .001f) return;
        // Shade only: leave lockscreen/bouncer and light rendering untouched.
        ClassLoader loader = (ClassLoader)call(context, "getClassLoader");
        Class<?> dependency = Class.forName("com.android.systemui.DependencyEx", false, loader);
        Object deps = field(dependency, "sDependency").get(null);
        Object central = call(deps, "getDependency", new Class<?>[]{Class.class},
                Class.forName("com.android.systemui.statusbar.phone.CentralSurfacesImpl", false, loader));
        if (field(central.getClass(), "mState").getInt(central) != 0) return;
        Object controller = field(central.getClass(), "mScrimController").get(central);
        if (!"UNLOCKED".equals(call(controller, "getState").toString())) return;
        int mask = token(context, "system_accent2_900");
        // Expanded ColorOS shade keeps behind_scrim at approximately half alpha.
        // Preserve its fade while reaching the requested179/255 additional tint.
        int alpha = Math.round(179 * Math.min(1f, visibility * 2f));
        paintColor.invoke(tintPaint, (mask & 0xffffff) | (alpha << 24));
        canvasRect.invoke(canvas, 0f, 0f, ((Integer)call(view, "getWidth")).floatValue(),
                ((Integer)call(view, "getHeight")).floatValue(), tintPaint);
        if (LOGGED.add("tint:" + mask + ":" + alpha)) log("dark final scrim: alpha=" + alpha
                + "/255 scrim=" + visibility + " Monet=" + String.format("#%08X", mask));
        if (LOGGED.add("palette:" + mask)) {
            for (String name : new String[]{"system_accent2_900", "system_neutral1_900",
                    "system_neutral1_800", "system_neutral1_50", "system_on_surface_dark"}) {
                log("SystemUI palette " + name + " " + String.format("#%08X", token(context, name)));
            }
        }
    }
    private static void solid(Object view) throws Exception {
        Object context = call(view, "getContext");
        boolean dark = night(context);
        int color = token(context, dark ? "system_neutral1_900" : "system_neutral1_50");
        float radius = progressRadius.getFloat(view);
        Surface surface = SURFACES.get(view);
        if (surface == null) {
            surface = new Surface(drawableCtor.newInstance());
            SURFACES.put(view, surface);
        }
        if (surface.color != color) {
            drawableColor.invoke(surface.drawable, color);
            surface.color = color;
        }
        if (surface.radius != radius) {
            drawableRadius.invoke(surface.drawable, radius);
            surface.radius = radius;
        }
        if (mixDrawing.getBoolean(view)) {
            // The original onDraw sets bounds and draws the original active blur fill afterward.
            // No background blur survives beneath this opaque drawable.
            baseDrawable.set(view, surface.drawable);
        } else {
            // Global themes use the superclass drawable branch before mBackgroundColor.
            // Its original active-track method remains untouched.
            drawableBounds.invoke(surface.drawable, backgroundRect.get(view));
            inactiveDrawable.set(view, surface.drawable);
        }
        String state = (dark ? "dark" : "light") + String.format(" #%08X", color);
        if (LOGGED.add(state)) log("solid slider " + state + " mix=" + mixDrawing.getBoolean(view)
                + " inactive=" + inactiveDrawable.get(view));
    }
    private static final class Surface {
        final Object drawable;
        int color;
        float radius = -1;
        Surface(Object drawable) { this.drawable = drawable; }
    }
    public static final class Callback {
        final boolean isStatic;
        final int action;
        volatile Method backup;
        Callback(Method target, int action) {
            this.isStatic = Modifier.isStatic(target.getModifiers());
            this.action = action;
        }
        private Object original(Object[] args) throws Throwable {
            try {
                return isStatic ? backup.invoke(null, args)
                        : backup.invoke(args[0], Arrays.copyOfRange(args, 1, args.length));
            } catch (InvocationTargetException error) { throw error.getCause(); }
        }
        public Object call(Object[] args) throws Throwable {
            if (action == 3) {
                Object result = original(args);
                try { tint(args[0], args[1]); } catch (Throwable error) { fail(error); }
                return result;
            }
            if (action == 0) {
                Object result = original(args);
                setup(args[1]);
                return result;
            }
            if (action == 1) {
                // RRO still supplies the night mask; this changes only blur mixing strength.
                if (Boolean.TRUE.equals(args[2])) {
                    args = args.clone();
                    args[2] = Boolean.FALSE;
                    if (LOGGED.add("panel")) log("dark panel mixing: using 80%-mask recipe");
                }
            } else if (action == 2) {
                try { solid(args[0]); } catch (Throwable error) { fail(error); }
            }
            return original(args);
        }
    }
}
