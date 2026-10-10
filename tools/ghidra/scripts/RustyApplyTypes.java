// Create structure types from tools/recomp/types/*.tsv and apply them where "#apply <program> <seg:off> <count>" says.
// Rows: offset<TAB>byte|word|dword<TAB>name<TAB>comment. Gaps stay undefined bytes.
// Args: <types directory>
// @category Rusty

import java.io.File;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.SegmentedAddress;
import ghidra.program.model.address.SegmentedAddressSpace;
import ghidra.program.model.data.ArrayDataType;
import ghidra.program.model.data.ByteDataType;
import ghidra.program.model.data.CategoryPath;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.DataTypeConflictHandler;
import ghidra.program.model.data.DWordDataType;
import ghidra.program.model.data.StructureDataType;
import ghidra.program.model.data.WordDataType;
import ghidra.program.model.mem.MemoryBlock;

public class RustyApplyTypes extends GhidraScript {

	private DataType primitive(String name) {
		switch (name) {
			case "word":
				return WordDataType.dataType;
			case "dword":
				return DWordDataType.dataType;
			default:
				return ByteDataType.dataType;
		}
	}

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		File directory = new File(args.length > 0 ? args[0] : "types");
		File[] files = directory.listFiles((dir, name) -> name.endsWith(".tsv"));
		if (files == null) {
			printerr("no types directory: " + directory);
			return;
		}
		int loadSegment = Integer.MAX_VALUE;
		for (MemoryBlock block : currentProgram.getMemory().getBlocks()) {
			if (block.getStart() instanceof SegmentedAddress && block.isInitialized()) {
				loadSegment = Math.min(loadSegment, ((SegmentedAddress) block.getStart()).getSegment());
			}
		}
		SegmentedAddressSpace space = (SegmentedAddressSpace) currentProgram.getAddressFactory().getDefaultAddressSpace();
		for (File file : files) {
			String structName = null;
			int size = 0;
			List<String[]> applies = new ArrayList<>();
			List<String[]> rows = new ArrayList<>();
			for (String line : Files.readAllLines(file.toPath())) {
				if (line.isBlank()) {
					continue;
				}
				String[] columns = line.split("\t", -1);
				if (columns[0].equals("#struct")) {
					structName = columns[1];
					size = Integer.decode(columns[2]);
				}
				else if (columns[0].equals("#apply")) {
					applies.add(columns);
				}
				else if (!line.startsWith("#")) {
					rows.add(columns);
				}
			}
			if (structName == null) {
				continue;
			}
			StructureDataType structure = new StructureDataType(new CategoryPath("/Rusty"), structName, size);
			for (String[] row : rows) {
				int offset = Integer.parseInt(row[0], 16);
				DataType type = primitive(row[1]);
				structure.replaceAtOffset(offset, type, type.getLength(), row[2], row.length > 3 ? row[3] : null);
			}
			DataType stored = currentProgram.getDataTypeManager().addDataType(structure,
				DataTypeConflictHandler.REPLACE_HANDLER);
			println(currentProgram.getName() + ": type " + structName + " (" + rows.size() + " fields)");
			for (String[] apply : applies) {
				if (!apply[1].equals(currentProgram.getName())) {
					continue;
				}
				String[] parts = apply[2].split(":");
				Address address = space.getAddress(Integer.parseInt(parts[0], 16) + loadSegment,
					Integer.parseInt(parts[1], 16));
				int count = Integer.parseInt(apply[3]);
				ArrayDataType array = new ArrayDataType(stored, count, stored.getLength());
				clearListing(address, address.add(array.getLength() - 1));
				createData(address, array);
				println(currentProgram.getName() + ": applied " + structName + "[" + count + "] at " + address);
			}
		}
	}
}
