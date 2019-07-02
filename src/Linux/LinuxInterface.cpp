/*
 * LinuxInterface.cpp
 *
 *  Created on: 29 Mar 2019
 *      Author: Christian
 */


#include "LinuxInterface.h"
#include "DataTransfer.h"

#include "GCodes/GCodeBuffer/GCodeBuffer.h"
#include "GCodes/GCodes.h"
#include "Platform.h"
#include "PrintMonitor.h"
#include "RepRap.h"
#include "RepRapFirmware.h"

#if HAS_LINUX_INTERFACE

LinuxInterface::LinuxInterface() : transfer(new DataTransfer()), wasConnected(false), numDisconnects(0),
	reportPause(false), gcodeReply(new OutputStack())
{
}

void LinuxInterface::Init()
{
	transfer->Init();
	transfer->StartNextTransfer();
}

void LinuxInterface::Spin()
{
	if (transfer->IsReady())
	{
		// Process incoming packets
		for (size_t i = 0; i < transfer->PacketsToRead(); i++)
		{
			const PacketHeader *packet = transfer->ReadPacket();
			if (packet == nullptr)
			{
				if (reprap.Debug(moduleLinuxInterface))
				{
					reprap.GetPlatform().Message(DebugMessage, "Error trying to read next SPI packet\n");
				}
				break;
			}

			if (packet->request >= (uint16_t)LinuxRequest::InvalidRequest)
			{
				INTERNAL_ERROR;
				return;
			}
			const LinuxRequest request = (LinuxRequest)packet->request;

			switch (request)
			{
			// Request the state of the G-Code buffers
			case LinuxRequest::GetState:
			{
				uint32_t busyChannels = 0;
				for (size_t i = 0; i < NumGCodeBuffers; i++)
				{
					const GCodeBuffer *gb = reprap.GetGCodes().GetGCodeBuffer(i);
					if (!gb->IsCompletelyIdle() || gb->MachineState().state != GCodeState::normal || gb->IsFileFinished())
					{
						busyChannels |= (1 << i);
					}
				}

				// There is no need to request a retransmission here.
				// If DCS does not update the status, it will automatically request it again next time
				(void)transfer->WriteState(busyChannels);
				break;
			}

			// Perform an emergency stop
			case LinuxRequest::EmergencyStop:
				reprap.EmergencyStop();
				break;

			// Reset the controller
			case LinuxRequest::Reset:
				reprap.GetPlatform().SoftwareReset((uint16_t)SoftwareResetReason::user);
				return;

			// Perform a G/M/T-code
			case LinuxRequest::Code:
			{
				size_t dataLength = packet->length;
				const char *data = transfer->ReadData(dataLength);
				const CodeHeader *header = reinterpret_cast<const CodeHeader*>(data);

				GCodeBuffer *gb = reprap.GetGCodes().GetGCodeBuffer((size_t)header->channel);
				if (gb->IsCompletelyIdle())
				{
					// Fill up the buffer so another code can be started
					gb->Put(data, dataLength, true);
				}
				else
				{
					// Still busy processing a file, request the code again
					if (reprap.Debug(moduleLinuxInterface))
					{
						reprap.GetPlatform().MessageF(DebugMessage, "Received code for busy channel %d\n", (int)header->channel);
					}
					transfer->ResendPacket(packet);
				}
				break;
			}

			// Get the object model of a specific module (TODO report real object model here instead of status responses)
			case LinuxRequest::GetObjectModel:
			{
				uint8_t module = transfer->ReadGetObjectModel();
				OutputBuffer *buffer = reprap.GetStatusResponse(module, ResponseSource::Generic);
				if (buffer != nullptr)
				{
					if (!transfer->WriteObjectModel(module, buffer))
					{
						// Failed to write the whole object model, try again later
						transfer->ResendPacket(packet);
						OutputBuffer::ReleaseAll(buffer);
					}
				}
				else
				{
					// No output buffer could be allocated, this means RRF is really short on memory
					transfer->ResendPacket(packet);
					OutputBuffer::ReleaseAll(buffer);
				}
				break;
			}

			// Set value in the object model
			case LinuxRequest::SetObjectModel:
			{
				size_t dataLength = packet->length;
				const char *data = transfer->ReadData(dataLength);
				// TODO implement this
				(void)data;
				break;
			}

			// Print has been started, set file print info
			case LinuxRequest::PrintStarted:
			{
				String<MaxFilenameLength> filename;
				StringRef filenameRef = filename.GetRef();
				transfer->ReadPrintStartedInfo(packet->length, filenameRef, fileInfo);
				reprap.GetPrintMonitor().SetPrintingFileInfo(filename.c_str(), fileInfo);
				reprap.GetGCodes().StartPrinting(true);
				break;
			}

			// Print has been stopped
			case LinuxRequest::PrintStopped:
			{
				const PrintStoppedReason reason = transfer->ReadPrintStoppedInfo();
				if (reason == PrintStoppedReason::normalCompletion)
				{
					// Just mark the print file as finished
					GCodeBuffer *gb = reprap.GetGCodes().GetGCodeBuffer((size_t)CodeChannel::file);
					gb->SetPrintFinished();
				}
				else
				{
					// Stop the print with the given reason
					reprap.GetGCodes().StopPrint((StopPrintReason)reason);
				}
				break;
			}

			// Macro file has been finished
			case LinuxRequest::MacroCompleted:
			{
				CodeChannel channel;
				bool error;
				transfer->ReadMacroCompleteInfo(channel, error);

				GCodeBuffer *gb = reprap.GetGCodes().GetGCodeBuffer((size_t)channel);
				gb->MachineState().SetFileFinished();
				break;
			}

			// Return heightmap as generated by G29 S0
			case LinuxRequest::GetHeightMap:
			{
				if (!transfer->WriteHeightMap())
				{
					transfer->ResendPacket(packet);
				}
				break;
			}

			// Set heightmap via G29 S1
			case LinuxRequest::SetHeightMap:
				transfer->ReadHeightMap();
				break;

			// Lock movement and wait for standstill
			case LinuxRequest::LockMovementAndWaitForStandstill:
			{
				CodeChannel channel;
				transfer->ReadLockUnlockRequest(channel);
				GCodeBuffer *gb = reprap.GetGCodes().GetGCodeBuffer((size_t)channel);
				if (!reprap.GetGCodes().LockMovementAndWaitForStandstill(*gb))
				{
					transfer->ResendPacket(packet);
				}
				break;
			}

			// Unlock everything
			case LinuxRequest::Unlock:
			{
				CodeChannel channel;
				transfer->ReadLockUnlockRequest(channel);
				GCodeBuffer *gb = reprap.GetGCodes().GetGCodeBuffer((size_t)channel);
				reprap.GetGCodes().UnlockAll(*gb);
				break;
			}

			// Invalid request
			default:
				INTERNAL_ERROR;
				break;
			}
		}

		// Deal with pause events
		if (reportPause)
		{
			if (transfer->WritePrintPaused(pauseFilePosition, pauseReason))
			{
				reportPause = false;
			}
		}

		// Deal with macro file requests
		bool reportMissing;
		for (size_t i = 0; i < NumGCodeBuffers; i++)
		{
			GCodeBuffer *gb = reprap.GetGCodes().GetGCodeBuffer(i);
			const CodeChannel channel = (CodeChannel)i;

			// Handle macro start requests
			const char *requestedMacroFile = gb->GetRequestedMacroFile(reportMissing);
			if (requestedMacroFile != nullptr)
			{
				if (transfer->WriteMacroRequest(channel, requestedMacroFile, reportMissing))
				{
					gb->RequestMacroFile(nullptr, false);
				}
			}
			// Handle file abort requests
			else if (gb->IsAbortRequested())
			{
				if (transfer->WriteAbortFileRequest(channel))
				{
					gb->AcknowledgeAbort();
				}
			}

			// Handle stack changes
			if (transfer->LinuxHadReset())
			{
				gb->ReportStack();
			}

			if (gb->IsStackEventFlagged())
			{
				if (transfer->WriteStackEvent(channel, gb->MachineState()))
				{
					gb->AcknowledgeStackEvent();
				}
			}
		}

		// Deal with code replies
		bool dataSent = true;
		while (!gcodeReply->IsEmpty() && dataSent)
		{
			MessageType type = gcodeReply->GetFirstItemType();
			OutputBuffer *buffer = gcodeReply->GetFirstItem();
			dataSent = !transfer->WriteCodeReply(type, buffer);
			gcodeReply->SetFirstItem(buffer);
		}

		// Start the next transfer
		transfer->StartNextTransfer();
		wasConnected = true;
	}
	else if (wasConnected && !transfer->IsConnected())
	{
		if (reprap.Debug(moduleLinuxInterface))
		{
			reprap.GetPlatform().Message(DebugMessage, "Lost connection to Linux\n");
		}

		wasConnected = false;
		numDisconnects++;

		// Don't cache messages if they cannot be sent
		if (!gcodeReply->IsEmpty())
		{
			gcodeReply->ReleaseAll();
		}

		// Close all open G-code files
		for (size_t i = 0; i < NumGCodeBuffers; i++)
		{
			reprap.GetGCodes().GetGCodeBuffer(i)->AbortFile(false);
		}
		reprap.GetGCodes().StopPrint(StopPrintReason::abort);
	}
}

void LinuxInterface::Diagnostics(MessageType mtype)
{
	reprap.GetPlatform().Message(mtype, "=== Linux interface ===\n");
	reprap.GetPlatform().MessageF(mtype, "Number of disconnects: %" PRIu32 "\n", numDisconnects);
	transfer->Diagnostics(mtype);
}

void LinuxInterface::HandleGCodeReply(MessageType mt, const char *reply)
{
	if (!transfer->IsConnected())
	{
		return;
	}

	OutputBuffer *buffer = gcodeReply->GetLastItem();
	if (buffer == nullptr || buffer->IsReferenced() || gcodeReply->GetLastItemType() != mt)
	{
		if (!OutputBuffer::Allocate(buffer))
		{
			// No more space available, stop here
			return;
		}
		gcodeReply->Push(buffer, mt);
	}
	buffer->cat(reply);
}

void LinuxInterface::HandleGCodeReply(MessageType mt, OutputBuffer *buffer)
{
	if (!transfer->IsConnected())
	{
		OutputBuffer::ReleaseAll(buffer);
		return;
	}

	gcodeReply->Push(buffer, mt);
}

#endif
