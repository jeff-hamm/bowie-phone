#pragma once

// https: // raw.githubusercontent.com/GadgetReboot/KS0835F_Phone_SLIC/refs/heads/main/Sketch/KS0835F_SLIC_Demo.ino

// Enable ringing functionality (requires SLIC hardware with F/R and RM pins)
//#define CAN_RING 1

#ifdef CAN_RING
#define F_R 22
#define RM 18
#endif

#define SHK 21
#define RING_CYCLE_MS 750
//#define ASSUME_HOOK 1
// PINOUT
// Row1:
// Row2: 21, 22, 18, 23
// 5, for sure, works as input. Maybe not for SD?

// 21 SHK works