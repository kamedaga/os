# LWJGL jars use Java's standard classpath wildcard expansion.
LWJGL_HOME=/opt/lwjgl
export LWJGL_HOME
case ":${CLASSPATH-}:" in
    *":$LWJGL_HOME/lib/*:"*) ;;
    *) CLASSPATH="${CLASSPATH:-.}:$LWJGL_HOME/lib/*" ;;
esac
export CLASSPATH
