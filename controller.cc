#include "controller.h"

#include "util.h"

Status Controller::Open() {
	RETURN_IF_ERROR(driver_->Open());
	return driver_->WriteTimedSequence(SequenceGenerator::INIT_SEQUENCE);
}

Status Controller::ReadDeviceId(uint16_t *device_id) {
    RETURN_IF_ERROR(LoadAddress(0x3ffffe));
    datastring bytes;
	RETURN_IF_ERROR(driver_->ReadWithCommand(TABLE_READ_post_inc, 2, &bytes));
    *device_id = bytes[0] | static_cast<uint16_t>(bytes[1]) << 8;
    return Status::OK;
}

Status Controller::ReadFlashMemory(uint32_t start_address, uint32_t end_address, datastring *result) {
	RETURN_IF_ERROR(LoadAddress(start_address));
	return driver_->ReadWithCommand(TABLE_READ_post_inc, end_address - start_address, result);
}

Status Controller::BulkErase() {
	RETURN_IF_ERROR(LoadAddress(0x3C0005));
	// 1100 0F 0F Write 0Fh to 3C0005h
	RETURN_IF_ERROR(driver_->WriteCommand(TABLE_WRITE, 0x0F0F));
	RETURN_IF_ERROR(LoadAddress(0x3C0004));
	datastring sequence;
	// 1100 8F 8F Write 8F8Fh TO 3C0004h to erase entire device.
	RETURN_IF_ERROR(driver_->WriteCommand(TABLE_WRITE, 0x8F8F));
	// 0000 00 00 NOP
	RETURN_IF_ERROR(driver_->WriteCommand(CORE_INST, 0x0000));
	// 0000 00 00 Hold PGD low until erase completes.
	return driver_->WriteTimedSequence(SequenceGenerator::BULK_ERASE_SEQUENCE);
}

Status Controller::WriteFlash(uint32_t address, const datastring &data) {
	if (address & 63) {
		FATAL("Attempting to write at a non-aligned address\n%s", "");
	}
	if (data.size() & 63)  {
		FATAL("Attempting to write less than one block of data\n%s", "");
	}
	// BSF EECON1, EEPGD
	RETURN_IF_ERROR(driver_->WriteCommand(CORE_INST, 0x8EA6));
	// BCF EECON1, CFGS
	RETURN_IF_ERROR(driver_->WriteCommand(CORE_INST, 0x9CA6));
	// BSF EECON1, WREN
	RETURN_IF_ERROR(driver_->WriteCommand(CORE_INST, 0x84A6));
	RETURN_IF_ERROR(LoadAddress(address));
	for (size_t i = 0; i < data.size(); i += 64) {
		for (size_t j = 0; j < 62; j += 2) {
			RETURN_IF_ERROR(driver_->WriteCommand(TABLE_WRITE_post_inc2, (static_cast<uint16_t>(data[i + j + 1]) << 8) | data[i + j]));
		}
		RETURN_IF_ERROR(driver_->WriteCommand(TABLE_WRITE_post_inc2_start_pgm, (static_cast<uint16_t>(data[i + 63]) << 8) | data[i + 62]));
		RETURN_IF_ERROR(driver_->WriteTimedSequence(SequenceGenerator::WRITE_SEQUENCE));
	}
	return Status::OK;
}

Status Controller::RowErase(uint32_t address) {
	if (address & 63) {
		FATAL("Attempting to erase a non-aligned address\n%s", "");
	}
	// BSF EECON1, EEPGD
	RETURN_IF_ERROR(driver_->WriteCommand(CORE_INST, 0x8EA6));
	// BCF EECON1, CFGS
	RETURN_IF_ERROR(driver_->WriteCommand(CORE_INST, 0x9CA6));
	// BSF EECON1, WREN,
	RETURN_IF_ERROR(driver_->WriteCommand(CORE_INST, 0x84A6));
	RETURN_IF_ERROR(LoadAddress(address));
	// BSF EECON1, FREE
	RETURN_IF_ERROR(driver_->WriteCommand(CORE_INST, 0x88A6));
	// BSR EECON1, WR
	RETURN_IF_ERROR(driver_->WriteCommand(CORE_INST, 0x82A6));
	// NOP
	RETURN_IF_ERROR(driver_->WriteCommand(CORE_INST, 0x0000));
	// NOP
	RETURN_IF_ERROR(driver_->WriteCommand(CORE_INST, 0x0000));

	// Loop until the WR bit in EECON1 is clear.
	datastring value;
	do {
		// MOVF EECON1, W, 0
		RETURN_IF_ERROR(driver_->WriteCommand(CORE_INST, 0x50A6));
		// MOVWF TABLAT
		RETURN_IF_ERROR(driver_->WriteCommand(CORE_INST, 0x6EF5));
		// NOP
		RETURN_IF_ERROR(driver_->WriteCommand(CORE_INST, 0x0000));
		// Read value from TABLAT
		RETURN_IF_ERROR(driver_->ReadWithCommand(SHIFT_OUT_TABLAT, 1, &value));
	} while (value[0] & 2);
	Sleep(MicroSeconds(200));
	// BCF EECON1, WREN
	return driver_->WriteCommand(CORE_INST, 0x9AA6);
}

Status Controller::LoadAddress(uint32_t address) {
	// MOVLW <first byte of address>
	RETURN_IF_ERROR(driver_->WriteCommand(CORE_INST, 0x0E00 | ((address >> 16) & 0xff)));
	// MOVWF TBLPTRU
	RETURN_IF_ERROR(driver_->WriteCommand(CORE_INST, 0x6EF8));
	// MOVLW <second byte of address>
	RETURN_IF_ERROR(driver_->WriteCommand(CORE_INST, 0x0E00 | ((address >> 8) & 0xff)));
	// MOVWF TBLPTRH
	RETURN_IF_ERROR(driver_->WriteCommand(CORE_INST, 0x6EF7));
	 // MOVLW <last byte of address>
	RETURN_IF_ERROR(driver_->WriteCommand(CORE_INST, 0x0E00 | (address & 0xff)));
	// MOVWF TBLPTRL
	return driver_->WriteCommand(CORE_INST, 0x6EF6);
}

Status HighLevelController::ReadProgram(Program *program) {
	DeviceCloser closer(this);
	RETURN_IF_ERROR(InitDevice());
	printf("Initialized device [%s]\n", device_info_.name.c_str());

	RETURN_IF_ERROR(ReadData(&(*program)[0], 0, device_info_.program_memory_size));
	RETURN_IF_ERROR(ReadData(&(*program)[device_info_.user_id_offset], device_info_.user_id_offset, device_info_.user_id_size));
	RETURN_IF_ERROR(ReadData(&(*program)[device_info_.config_offset], device_info_.config_offset, device_info_.config_size));
	return Status::OK;
}

Status HighLevelController::InitDevice() {
	if (device_open_) {
		return Status::OK;
	}
	Status status;
	uint16_t device_id;
	for (int attempts = 0; attempts < 10; ++attempts) {
		status = controller_->Open();
		if (!status.ok()) {
			controller_->Close();
			continue;
		}
		status = controller_->ReadDeviceId(&device_id);
		if (!status.ok() || device_id == 0) {
			controller_->Close();
			continue;
		}
		status = device_db_->GetDeviceInfo(device_id, &device_info_);
		if (status.ok()) {
			device_open_ = true;
		}
		return status;
	}
	return Status(Code::DEVICE_NOT_FOUND, "No device detected");
}

void HighLevelController::CloseDevice() {
	if (!device_open_) {
		return;
	}
	device_open_ = false;
	controller_->Close();
}

Status HighLevelController::ReadData(datastring *data, uint32_t base_address, uint32_t target_size) {
	data->reserve(target_size);
	printf("Starting read at address %06lX to read %06X bytes\n", base_address + data->size(), target_size);
	while (data->size() < target_size) {
		datastring buffer;
		uint32_t start_address = base_address + data->size();
		Status status = controller_->ReadFlashMemory(start_address, start_address + std::min<uint32_t>(128, target_size - data->size()), &buffer);
		if (status.ok()) {
			data->append(buffer);
		} else if (status.code() != Code::SYNC_LOST) {
			return status;
		}
	}
	return Status::OK;
}