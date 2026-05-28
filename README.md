# Automated Parking System

## Group information
### Team members
- Adrish Danka 
- Arjunraj Sasikumar 
- Fadil Imran Pasha

## Project Description
Our project in an automated parking system with slot anomaly detection. It manages parking access, slot availability, 
duration logging and cost calculations, and anomaly detection within the slots. The system regulates access using RFID tags 
given to cars registered in the parking lot. Slot availability is also monitored through card taps and indicated through LEDs.
Duration logging and cost calculation is also done by keeping record of RFID taps. Emergency vehicles are issued a special
RFID card that grants them access even if the parking lot is full; normal cars are not allowed to enter if parking lot is full.

We also have an anomaly detction system implemneted using ultrasonic distance sensors. Any sudden movement is flagged and if the
state persists for a long time, we detect an anomaly and the parking lot is sealed. Normal cars cannot enter or leave until the emergency
is cleared. The RGB LED flashes and a message is displayed on LCD. Once normal distance is recorded or a police car arrives and handles
the emergency, normal operations are resumed.

This system can be scaled up and implemented in parking lots to reduce operation costs, increase user convienience, and serve as an alert
system for parking authorities in case an accident or mishap occurs in the lot.

## Video
<p align="center">
  <img width="757" height="515" alt="Demo Preview" src="assets/demo.gif" />
</p>

## System Diagram
<p align="center">
  <img width="757" height="515" alt="Project Wokwi Diagram" src="assets/0.png" />
</p>

## How it works
The task interaction diagram is shown below.

```mermaid
flowchart LR
    %% Class Definitions for neat styling
    classDef task fill:#e3f2fd,stroke:#1e88e5,stroke-width:2px,color:#000
    classDef resource fill:#fff8e1,stroke:#f9a825,stroke-width:2px,color:#000
    classDef ipc fill:#f3e5f5,stroke:#8e24aa,stroke-width:1px,stroke-dasharray: 5 5,color:#000
    classDef hardware fill:#eeeeee,stroke:#616161,stroke-width:2px,color:#000

    subgraph Input ["Input Stage"]
        direction TB
        RFID["<b>RFIDScanner</b> <br>Calls on_picc_state_changed when a card is scanned"]:::hardware
        T3["<b>Anomaly Detection Task</b><br>Polls ultrasonics every 500ms;<br>flags crash if shift held for 2s"]:::task
    end

    subgraph Processing ["Processing Stage"]
        direction TB
        T1["<b>Entry Access Control Task</b><br>Checks space & emergency mode;<br>adds session; decrements slot"]:::task
        T2["<b>Exit Access Control Task</b><br>Removes session; computes cost;<br>increments slot"]:::task
        T5["<b>Emergency Response Task</b><br>Sets emergencyMode flag;<br>handles police RFID entry & exit"]:::task
        SP[(🔒 Shared Parking Data<br>g_sessions, g_availableSpaces)]:::resource
    end

    subgraph Actuation ["Actuation Stage"]
        direction TB
        T6["<b>Barrier Control Task</b><br>Rotates servo 0→90° for 3s then closes"]:::task
        T7["<b>LCD Display Task</b><br> Writes to display"]:::task
        T8["<b>LED Indicator & Alert Task</b><br>RGB + slot LEDs;<br>flashes red/blue in emergency"]:::task
    end

    %% IPC Nodes (Queues & Semaphores) to consolidate arrows
    qRFID(["entryRFIDQueue / exitRFIDQueue"]):::ipc
    qGate(["gateQueue"]):::ipc
    qLCD(["lcdQueue"]):::ipc
    semLED(["notifyLEDsBinSem"]):::ipc
    semCrash(["accidentDetectedBinSem /<br>flashLEDBinSem"]):::ipc

    %% Connections: Input -> Processing
    RFID --> qRFID
    qRFID --> T1 & T2
    
    T3 -.-> semCrash
    semCrash -.-> T5

    %% Connections: Shared Data Access
    T1 & T2 & T5 <--> |"parkingMutex"| SP

    %% Connections: Processing -> Actuation
    T1 & T2 --> qGate
    qGate --> T6
    
    T1 & T2 & T5 --> qLCD
    qLCD --> T7
    
    T1 & T2 & T5 -.-> semLED
    semLED -.-> T8
```
```mermaid
flowchart TD
    subgraph Key [Key]
        %% Row 1 (Top invisible anchors)
        t1[ ] ~~~ q1[ ] ~~~ s1[ ] ~~~ p1[ ] ~~~ l1[ ] ~~~ sr1[ ]
        
        %% Vertical columns
        t1 ~~~ Task[Task] ~~~ t2[ ]
        q1 -- "Queue used to<br/>pass data" --> q2[ ]
        s1 -. "Binary Sem:<br/>used to signal" .-> s2[ ]
        p1 -- "Tasks contending for<br/>shared Parking data<br/>structure" <--> p2[ ]
        l1 -- "Tasks contending for<br/>LCD Display" <--> l2[ ]
        sr1 ~~~ Shared[🔒 Shared Resource] ~~~ sr2[ ]
        
        %% Row 3 (Bottom invisible anchors)
        t2 ~~~ q2 ~~~ s2 ~~~ p2 ~~~ l2 ~~~ sr2
    end

    %% --- STYLING ---
    
    %% Background and Borders
    style Key fill:#FFFDE7,stroke:#D4C985,stroke-width:2px
    style Task fill:#E1D5E7,stroke:#9673A6,stroke-width:1px
    style Shared fill:#E1D5E7,stroke:#9673A6,stroke-width:1px

    %% Hide the helper/anchor nodes completely
    classDef inv fill:none,stroke:none;
    class t1,t2,q1,q2,s1,s2,p1,p2,l1,l2,sr1,sr2 inv;

    %% Precise Arrow Styling (using link sequential indexes)
    linkStyle 7 stroke:#000000,stroke-width:2px
    linkStyle 8 stroke:#000000,stroke-width:2px,stroke-dasharray: 5 5
    linkStyle 9 stroke:#2E7D32,stroke-width:2px
    linkStyle 10 stroke:#C62828,stroke-width:2px
```

## Project Imapact
Our automated parking system addresses real world inefficiencies in traditional parking management by replacing manual operations with a fully automated, intelligent solution.

`Operational Cost Reduction` - By automating entry, exit, and slot monitoring, the system eliminates the need for on-site parking staffs, significantly cutting labor costs for parking facility operators.

`Improved User Convenience` - RFID based access allows registered vehicles to enter and exit quickly without stopping to interact with staff or ticketing machines. Real-time LED indicators let drivers instantly know slot availability, reducing time spent searching for a space.

`Automated Billing` - Duration logging and automatic cost calculation remove the need for manual fare collection, reducing human error and making the payment process seamless.

`Enhanced Safety and Emergency Response` - The anomaly detection system using ultrasonic sensors provides an added layer of security. By detecting sudden unusual movement and immediately sealing the lot, the system can contain a potential accident or security threat before it escalates. Authorities are alerted through visual and display indicators, enabling faster response.

`Priority Access for Emergency Vehicles` - The special RFID privilege for emergency vehicles ensures that ambulances and police cars are never blocked, even during a full lot or an active emergency, a critical feature in life threatening situations.

## FreeRTOS Implementation

The project uses FreeRTOS to divide the parking system into separate tasks. Each task handles one main part of the system, such as RFID access control, barrier movement, LCD messages, LED indicators, emergency handling, and ultrasonic anomaly detection. This makes the program easier to organize and allows different parts of the system to run independently.

The RFID scanner detects scanned cards and the scanned tag is then routed into either the entry RFID queue or the exit RFID queue depending on whether the vehicle is already inside the parking lot.

### Tasks

| Task Name | Priority | Description | Timing / Constraint |
|---|---|---|---|
| `vLCDDisplayTask` | 3 | Receives LCD messages from `lcdQueue` and displays them on the LCD. Messages are formatted into two lines. | Waits until a new LCD message is received, then updates the display. |
| `vLEDIndicatorTask` | 3 | Controls the normal parking LEDs and RGB LED. In normal mode, LEDs show available spaces. In emergency mode, the RGB LED flashes red and blue. | Updates when `notifyLEDsBinSem` is given, and flashes periodically during emergency mode. |
| `vEmergencyResponseTask` | 6 | Handles the emergency/anomaly state. It waits for `accidentDetectedBinSem`, sets emergency mode, displays an emergency LCD message, blocks normal access, and clears emergency mode when the anomaly stops. It also handles the emergency/police RFID behavior. | Highest priority task because emergency handling should override normal parking operation. |
| `vEntryAccessControlTask` | 5 | Handles RFID-based entry. It checks the tag type, whether the car is already inside, whether the lot is full, and whether emergency mode is active. If entry is allowed, it adds a parking session, decreases available spaces for normal cars, sends a barrier command, updates the LCD, and notifies the LED task. | Should respond quickly after a valid entry RFID scan. |
| `vExitAccessControlTask` | 5 | Handles RFID-based exit. It checks whether the scanned tag belongs to a vehicle currently inside the parking lot. If exit is allowed, it removes the parking session, increases available spaces for normal cars, sends a barrier command, and updates the LCD. The duration logging and cost calculation are also handled inside this task using the stored entry tick. | Should respond quickly after an exit RFID scan and update parking availability immediately. |
| `vBarrierControlTask` | 4 | Controls the servo barrier. It receives commands from `gateQueue`, opens the barrier using the servo motor, keeps it open briefly, and then closes it again. | Opens the barrier for about 3 seconds, then closes it and waits briefly before releasing control. |
| `AnomalyDetectionTask` | 7 | Reads the ultrasonic sensors and checks for sudden distance changes in the parking slots. If abnormal movement remains for the required time, it signals an accident/anomaly using semaphores. | Runs periodically, with sensor checks approximately every 500 ms. An anomaly is confirmed only if the sudden movement persists for the defined time. |

### Task Communication and Synchronization

The system uses FreeRTOS queues to pass events or commands between tasks. It uses semaphores to signal events, and mutexes to protect shared resources such as the LCD, barrier, and parking data.

| FreeRTOS Object | Type | Purpose |
|---|---|---|
| `entryRFIDQueue` | Queue | Stores RFID tag IDs that should be handled by the entry access-control task. |
| `exitRFIDQueue` | Queue | Stores RFID tag IDs that should be handled by the exit access-control task. |
| `gateQueue` | Queue | Sends gate commands to the barrier task, such as opening the barrier. |
| `lcdQueue` | Queue | Sends LCD messages to the LCD display task. |
| `accidentDetectedBinSem` | Binary Semaphore | Signals the emergency response task that an anomaly or accident has been detected. |
| `flashLEDBinSem` | Binary Semaphore | Signals the LED task to flash the RGB LED during emergency/anomaly mode. |
| `notifyLEDsBinSem` | Binary Semaphore | Notifies the LED task to refresh the LED indicators after parking availability changes or after emergency mode clears. |
| `lcdMutex` | Mutex | Protects the LCD so that only one task writes to it at a time. |
| `parkingMutex` | Mutex | Protects shared parking data, including available spaces, active parking sessions, and emergency/police RFID state. |
| `barrierMutex` | Mutex | Protects the servo barrier so that only one gate operation happens at a time. |

During normal entry, the RFID reads the scanned tag and routes it to `entryRFIDQueue` if the vehicle is not already inside. The entry access task then checks the tag type, parking availability, and emergency state. If entry is allowed, it updates the parking session, sends an open-barrier command to `gateQueue`, sends a message to `lcdQueue`, and gives `notifyLEDsBinSem` so the LEDs can update.

During normal exit, the scanned tag is routed to `exitRFIDQueue`. The exit access task checks the stored parking session, calculates how long the car stayed using the saved entry tick, calculates the cost, removes the session, updates the available-space count, sends a barrier command, and updates the LCD.

During an anomaly, `AnomalyDetectionTask` detects abnormal ultrasonic readings and gives `accidentDetectedBinSem` and `flashLEDBinSem`. The emergency response task then sets emergency mode, blocks normal access, displays an emergency message on the LCD, and keeps the system in alert mode until the anomaly is cleared or handled.

## Screenshots

1. Initial Parking System State: when nothing has happened yet.

<table>
  <tr>
    <td align="center">
      <img width="420" alt="Initial parking system state" src="assets/1.png" />
    </td>
    <td align="center">
      <img width="420" alt="Initial LCD/status output" src="assets/1.1.png" />
    </td>
  </tr>
</table>

2. One car allowed, thus using one parking slot.

<table>
  <tr>
    <td align="center">
      <img width="420" alt="One car access granted" src="assets/2.png" />
    </td>
    <td align="center">
      <img width="420" alt="One parking slot occupied" src="assets/2.1.png" />
    </td>
  </tr>
</table>

3. Another car allowed, thus using both parking slots.

<table>
  <tr>
    <td align="center">
      <img width="420" alt="Second car access granted" src="assets/3.png" />
    </td>
    <td align="center">
      <img width="420" alt="Parking lot full" src="assets/3.1.png" />
    </td>
  </tr>
</table>

4. Anomaly detected state.

<table>
  <tr>
    <td align="center">
      <img width="420" alt="Anomaly detected state" src="assets/4.png" />
    </td>
  </tr>
</table>

5. Police entry and exit during emergency mode.

<table>
  <tr>
    <td align="center">
      <img width="420" alt="Police entry during emergency mode" src="assets/5.png" />
    </td>
    <td align="center">
      <img width="420" alt="Police exit during emergency mode" src="assets/5.1.png" />
    </td>
  </tr>
</table>

6. Car trying to leave/enter during emergency mode.

<table>
  <tr>
    <td align="center">
      <img width="420" alt="Vehicle blocked during emergency mode" src="assets/6.png" />
    </td>
    <td align="center">
      <img width="420" alt="Emergency mode access restriction" src="assets/6.1.png" />
    </td>
  </tr>
</table>

7. Normal mode after emergency mode.

<table>
  <tr>
    <td align="center">
      <img width="420" alt="Normal mode after emergency mode" src="assets/7.png" />
    </td>
  </tr>
</table>

8. Car exiting states.

<table>
  <tr>
    <td align="center">
      <img width="420" alt="Car exiting state" src="assets/8.png" />
    </td>
    <td align="center">
      <img width="420" alt="Updated parking state after exit" src="assets/8.1.png" />
    </td>
  </tr>
</table>
