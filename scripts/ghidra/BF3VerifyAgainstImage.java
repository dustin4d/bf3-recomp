// Compare the current program's loaded memory with flat PE images (rex_extract image output).
// Arguments: one or more [<space>=]<image.bin>. Each image maps at the ImageBase in its own PE
// header, in the default space or the named overlay space (BF3: BF_Splash=...BF.Splash.image.bin).
// Read-only. Prints one REPORT line per checked block, a VERIFY line per image, and a summary.
//@category BF3
//@author bf3-recomp

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.nio.file.Path;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.AddressSpace;
import ghidra.program.model.mem.MemoryBlock;

public class BF3VerifyAgainstImage extends GhidraScript {

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		if (args.length == 0) {
			throw new IllegalArgumentException("usage: BF3VerifyAgainstImage [<space>=]<image.bin>...");
		}
		long totalBad = 0;
		for (String arg : args) {
			String spaceName = null, path = arg;
			int eq = arg.indexOf('=');
			if (eq > 0) {
				spaceName = arg.substring(0, eq);
				path = arg.substring(eq + 1);
			}
			AddressSpace space = spaceName == null
					? currentProgram.getAddressFactory().getDefaultAddressSpace()
					: currentProgram.getAddressFactory().getAddressSpace(spaceName);
			if (space == null) {
				throw new IllegalArgumentException("no address space " + spaceName);
			}
			totalBad += verify(Path.of(path), space);
		}
		println(String.format("VERIFY %s: %s", currentProgram.getName(),
			totalBad == 0 ? "all images match" : totalBad + " bytes differ"));
		println(String.format("VERIFY %d functions, %d memory blocks",
			currentProgram.getFunctionManager().getFunctionCount(),
			currentProgram.getMemory().getBlocks().length));
	}

	private long verify(Path path, AddressSpace space) throws Exception {
		byte[] image = Files.readAllBytes(path);
		// The loader doesn't set the program image base to the XEX base; take it from the PE header.
		ByteBuffer le = ByteBuffer.wrap(image).order(ByteOrder.LITTLE_ENDIAN);
		long base = Integer.toUnsignedLong(le.getInt(le.getInt(0x3C) + 0x34));
		long checked = 0, mismatched = 0;
		for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
			if (!b.isInitialized() || !b.getStart().getAddressSpace().equals(space)) {
				continue;
			}
			long off = b.getStart().getOffset() - base;
			if (off < 0 || off >= image.length) {
				continue;  // another module's block
			}
			long len = Math.min(b.getSize(), image.length - off);
			byte[] bytes = new byte[(int) len];
			b.getBytes(b.getStart(), bytes);
			long bad = 0, first = -1;
			for (int i = 0; i < bytes.length; i++) {
				if (bytes[i] != image[(int) off + i]) {
					if (first < 0) {
						first = i;
					}
					bad++;
				}
			}
			checked += len;
			mismatched += bad;
			String clipped = len < b.getSize()
					? String.format(" (block runs 0x%X past image end, unchecked)", b.getSize() - len)
					: "";
			println(String.format("REPORT %-24s %s size 0x%X: %s%s", b.getName(), b.getStart(),
				b.getSize(), bad == 0 ? "match"
						: String.format("%d bytes differ, first at %s", bad, b.getStart().add(first)),
				clipped));
		}
		println(String.format("VERIFY %s @ %s:0x%X: %d bytes checked, %d differ",
			path.getFileName(), space.getName(), base, checked, mismatched));
		return mismatched;
	}
}
