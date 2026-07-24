// Ghidra headless script (Java): apply System.map symbols, disassemble+create target
// functions, decompile them to C. args: <System.map> <comma,names> <outdir>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.*;
import java.util.*;

public class RknandRE extends GhidraScript {
    public void run() throws Exception {
        String[] a = getScriptArgs();
        String mapPath = a[0];
        Set<String> targets = new HashSet<>(Arrays.asList(a[1].split(",")));
        String outdir = a[2];
        new File(outdir).mkdirs();
        AddressSpace sp = currentProgram.getAddressFactory().getDefaultAddressSpace();
        Map<String,Address> targAddr = new LinkedHashMap<>();
        int labels = 0;
        BufferedReader br = new BufferedReader(new FileReader(mapPath));
        String line;
        while ((line = br.readLine()) != null) {
            String[] p = line.trim().split("\\s+");
            if (p.length < 3) continue;
            Address ad;
            try { ad = sp.getAddress(Long.parseLong(p[0], 16)); } catch (Exception e) { continue; }
            try { createLabel(ad, p[2], true); labels++; } catch (Exception e) {}
            if (targets.contains(p[2])) targAddr.put(p[2], ad);
        }
        br.close();
        println("[RE] labels=" + labels + " targets=" + targAddr.size());
        for (Map.Entry<String,Address> e : targAddr.entrySet()) {
            try { disassemble(e.getValue()); createFunction(e.getValue(), e.getKey()); } catch (Exception ex) {}
        }
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        ConsoleTaskMonitor mon = new ConsoleTaskMonitor();
        FunctionManager fm = currentProgram.getFunctionManager();
        int ok = 0;
        for (Map.Entry<String,Address> e : targAddr.entrySet()) {
            Function fn = fm.getFunctionAt(e.getValue());
            if (fn == null) continue;
            try {
                DecompileResults r = di.decompileFunction(fn, 120, mon);
                if (r != null && r.decompileCompleted()) {
                    String c = r.getDecompiledFunction().getC();
                    PrintWriter w = new PrintWriter(new File(outdir, e.getKey() + ".c"));
                    w.print(c); w.close(); ok++;
                }
            } catch (Exception ex) {}
        }
        println("[RE] decompiled=" + ok);
    }
}
