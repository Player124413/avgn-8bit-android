# The JNI bridge is looked up by name from C (FindClass + GetStaticMethodID), so keep it.
-keep class ru.avgn8bit.Native { *; }
-keepclasseswithmembernames class * { native <methods>; }
