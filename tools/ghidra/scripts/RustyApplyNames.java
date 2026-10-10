// Apply names from tools/recomp/names/<program>.tsv: "segment:offset<TAB>kind<TAB>name<TAB>comment".
// The segment is relative to the file image; the script adds the load segment of the program in Ghidra.
// kind: func (create or rename a function, plate comment), label/data (label, EOL comment), comment (EOL comment only).
// Args: <names directory>
// @category Rusty

import java.io.File;
import java.nio.file.Files;
import java.util.List;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.SegmentedAddress;
import ghidra.program.model.address.SegmentedAddressSpace;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.SourceType;

public class RustyApplyNames extends GhidraScript {

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		File file = new File(args.length > 0 ? args[0] : "names", currentProgram.getName() + ".tsv");
		if (!file.exists()) {
			println(currentProgram.getName() + ": no names file");
			return;
		}
		int loadSegment = Integer.MAX_VALUE;
		for (MemoryBlock block : currentProgram.getMemory().getBlocks()) {
			if (block.getStart() instanceof SegmentedAddress && block.isInitialized()) {
				loadSegment = Math.min(loadSegment, ((SegmentedAddress) block.getStart()).getSegment());
			}
		}
		SegmentedAddressSpace space = (SegmentedAddressSpace) currentProgram.getAddressFactory().getDefaultAddressSpace();
		List<String> lines = Files.readAllLines(file.toPath());
		int functions = 0;
		int labels = 0;
		for (String line : lines) {
			if (line.isBlank() || line.startsWith("#")) {
				continue;
			}
			String[] columns = line.split("\t", -1);
			String[] parts = columns[0].split(":");
			int segment = Integer.parseInt(parts[0], 16) + loadSegment;
			int offset = Integer.parseInt(parts[1], 16);
			Address address = space.getAddress(segment, offset);
			String kind = columns[1];
			String name = columns[2];
			String comment = columns.length > 3 ? columns[3] : "";
			if (kind.equals("func")) {
				Function function = getFunctionAt(address);
				if (function == null) {
					if (getInstructionAt(address) == null) {
						disassemble(address);
					}
					function = createFunction(address, name);
				}
				if (function == null) {
					printerr("cannot create function at " + address + " (" + name + ")");
					continue;
				}
				function.setName(name, SourceType.USER_DEFINED);
				if (!comment.isEmpty()) {
					setPlateComment(address, comment);
				}
				functions++;
			}
			else if (kind.equals("comment")) {
				setEOLComment(address, comment);
				labels++;
			}
			else {
				createLabel(address, name, true, SourceType.USER_DEFINED);
				if (!comment.isEmpty()) {
					setEOLComment(address, comment);
				}
				labels++;
			}
		}
		println(String.format("%s: %d functions, %d labels (load segment %04x)", currentProgram.getName(), functions,
			labels, loadSegment));
	}
}
