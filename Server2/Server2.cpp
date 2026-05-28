#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>      
#include <ws2tcpip.h>      
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <thread>          
#include <mutex>           
#include <ctime>
#include <algorithm>
#include <sstream>

#pragma comment(lib, "ws2_32.lib")

using namespace std;

const int PORT = 12345;            
const int BUF_SIZE = 4096;        


struct ClientInfo { SOCKET sock; string name; string room; };
struct ChatMessage { string sender; string message; string timestamp; };


vector<ClientInfo> clients;               
map<string, vector<ChatMessage>> chatHistory; 
mutex g_mutex;                              
int clientCounter = 1;                      


string GetTimestamp() {
    time_t t = time(nullptr);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime(&t));
    return buf;
}


string GetExeDirectory() {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    string full(path);
    size_t pos = full.find_last_of("\\/");
    if (pos != string::npos) return full.substr(0, pos + 1);
    return "";
}


int CountSpacesInFile(const string& filename) {
    string fullPath = GetExeDirectory() + filename;
    ifstream f(fullPath);
    if (!f.is_open()) return -1;   
    int cnt = 0;
    char ch;
    while (f.get(ch)) if (ch == ' ') cnt++;
    return cnt;
}

void SaveHistoryToFile(const string& room) {
    string fname = "chat_history_" + room + ".txt";
    ofstream f(fname);
    lock_guard<mutex> lock(g_mutex);   
    if (f) {
        f << "=== История комнаты " << room << " ===\n";
        for (auto& msg : chatHistory[room])
            f << "[" << msg.timestamp << "] " << msg.sender << ": " << msg.message << "\n";
    }
}

void BroadcastToAll(const string& msg, SOCKET exclude = INVALID_SOCKET) {
    lock_guard<mutex> lock(g_mutex);
    for (auto& c : clients)
        if (c.sock != exclude)
            send(c.sock, msg.c_str(), msg.length() + 1, 0);
}

void BroadcastToRoom(const string& room, const string& msg, SOCKET exclude = INVALID_SOCKET) {
    lock_guard<mutex> lock(g_mutex);
    for (auto& c : clients)
        if (c.room == room && c.sock != exclude)
            send(c.sock, msg.c_str(), msg.length() + 1, 0);
}


void HandleClient(SOCKET clientSock, string clientName) {
    string clientRoom = "main";  


    {
        lock_guard<mutex> lock(g_mutex);
        clients.push_back({ clientSock, clientName, clientRoom });
    }


    string welcome = "     Добро пожаловать, " + clientName + "! Ваша комната: main. Введите /help для справки.";
    send(clientSock, welcome.c_str(), welcome.length() + 1, 0);

    char buffer[BUF_SIZE];
    while (true) {
        memset(buffer, 0, BUF_SIZE);
        int bytes = recv(clientSock, buffer, BUF_SIZE - 1, 0);
        if (bytes <= 0) break; 
        string cmd(buffer);

        cmd.erase(remove(cmd.begin(), cmd.end(), '\r'), cmd.end());
        cmd.erase(remove(cmd.begin(), cmd.end(), '\n'), cmd.end());
        cout << "[MSG] от " << clientName << ": " << cmd << endl;


        if (cmd == "/exit") {
            break;
        }
        else if (cmd.rfind("/room ", 0) == 0) {       
            clientRoom = cmd.substr(6);
            lock_guard<mutex> lock(g_mutex);
            for (auto& c : clients)
                if (c.sock == clientSock) c.room = clientRoom;
            string resp = "Вы перешли в комнату " + clientRoom;
            send(clientSock, resp.c_str(), resp.length() + 1, 0);
        }
        else if (cmd.rfind("/spaces ", 0) == 0) {   
            string fname = cmd.substr(8);
            int spaces = CountSpacesInFile(fname);
            ostringstream oss;
            if (spaces >= 0)
                oss << "Пробелов в файле " << fname << ": " << spaces;
            else
                oss << "Ошибка: файл " << fname << " не найден";
            string resp = oss.str();
            send(clientSock, resp.c_str(), resp.length() + 1, 0);
        }
        else if (cmd.rfind("/sendfile ", 0) == 0) {   
            string fname = cmd.substr(10);
  
            DWORD fileSize;
            int bytes = recv(clientSock, (char*)&fileSize, sizeof(fileSize), 0);
            if (bytes != sizeof(fileSize)) continue;
    
            string savePath = GetExeDirectory() + "received_" + fname;
            HANDLE hFile = CreateFileA(savePath.c_str(), GENERIC_WRITE, 0, NULL,
                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hFile == INVALID_HANDLE_VALUE) {
                string err = "Ошибка сохранения файла";
                send(clientSock, err.c_str(), err.length() + 1, 0);
                continue;
            }
            DWORD total = 0, br;
            while (total < fileSize) {
                DWORD chunk = min((DWORD)BUF_SIZE, fileSize - total);
                bytes = recv(clientSock, buffer, chunk, 0);
                if (bytes <= 0) break;
                WriteFile(hFile, buffer, bytes, &br, NULL);
                total += bytes;
            }
            CloseHandle(hFile);
            string ok = "Файл " + fname + " сохранён на сервере";
            send(clientSock, ok.c_str(), ok.length() + 1, 0);
        }
        else if (cmd.rfind("/getfile ", 0) == 0) {    
            string fname = cmd.substr(9);
            string fullPath = GetExeDirectory() + fname;
            HANDLE hFile = CreateFileA(fullPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hFile == INVALID_HANDLE_VALUE) {      
                DWORD zero = 0;
                send(clientSock, (char*)&zero, sizeof(zero), 0);
                string err = "Файл " + fname + " не найден";
                send(clientSock, err.c_str(), err.length() + 1, 0);
                continue;
            }
            DWORD fileSize = GetFileSize(hFile, NULL);
           
            send(clientSock, (char*)&fileSize, sizeof(fileSize), 0);
            DWORD br;
            while (ReadFile(hFile, buffer, BUF_SIZE, &br, NULL) && br > 0)
                send(clientSock, buffer, br, 0);
            CloseHandle(hFile);
        }
        else if (cmd.rfind("/save ", 0) == 0) {      
            string room = cmd.substr(6);
            SaveHistoryToFile(room);
            string resp = "История комнаты " + room + " сохранена в файл";
            send(clientSock, resp.c_str(), resp.length() + 1, 0);
        }
        else if (cmd.rfind("/view ", 0) == 0) {       
            string room = cmd.substr(6);
            string fname = "chat_history_" + room + ".txt";
            ifstream f(fname);
            string content;
            if (f) {
                string line;
                while (getline(f, line)) content += line + "\n";
            }
            if (content.empty()) content = "История комнаты " + room + " пуста";
            send(clientSock, content.c_str(), content.length() + 1, 0);
        }
        else if (cmd.rfind("/broad ", 0) == 0) {     
            string msg = cmd.substr(7);
            string full = "[BROADCAST] " + clientName + ": " + msg;
            BroadcastToAll(full);
            string resp = "Сообщение разослано всем (" + to_string(clients.size()) + " клиентам)";
            send(clientSock, resp.c_str(), resp.length() + 1, 0);
        }
        else if (cmd == "/help") {                    
            string help =
                "\n=== КОМАНДЫ ===\n"
                " <текст>            - отправить сообщение в свою комнату\n"
                " /room <название>   - сменить комнату\n"
                " /spaces <файл>     - подсчитать пробелы (файл на сервере)\n"
                " /sendfile <путь>   - отправить файл на сервер\n"
                " /getfile <имя>     - скачать файл с сервера\n"
                " /save <комната>    - сохранить историю комнаты в файл\n"
                " /view <комната>    - показать сохранённую историю\n"
                " /broad <сообщение> - отправить всем клиентам\n"
                " /exit              - выход\n"
                "===================\n";
            send(clientSock, help.c_str(), help.length() + 1, 0);
        }
        else {   
            ChatMessage msg{ clientName, cmd, GetTimestamp() };
            string formatted = "[" + msg.timestamp + "] " + clientName + ": " + cmd;
            {
                lock_guard<mutex> lock(g_mutex);
                chatHistory[clientRoom].push_back(msg);
            }

            BroadcastToRoom(clientRoom, formatted, clientSock);
            send(clientSock, formatted.c_str(), formatted.length() + 1, 0);
        }
    }


    {
        lock_guard<mutex> lock(g_mutex);
        for (auto it = clients.begin(); it != clients.end(); ++it)
            if (it->sock == clientSock) { clients.erase(it); break; }
    }
    closesocket(clientSock);
    cout << "[-] " << clientName << " отключился" << endl;
}

int main() {

    SetConsoleCP(1251); SetConsoleOutputCP(1251);

  
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);


    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;   
    bind(listenSock, (sockaddr*)&addr, sizeof(addr));
    listen(listenSock, SOMAXCONN);  

    cout << "=== СЕРВЕР ЗАПУЩЕН на порту " << PORT << " ===" << endl;


    while (true) {
        sockaddr_in clientAddr;
        int len = sizeof(clientAddr);
        SOCKET clientSock = accept(listenSock, (sockaddr*)&clientAddr, &len);
        if (clientSock == INVALID_SOCKET) continue;

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, ip, INET_ADDRSTRLEN);
        cout << "[+] Новый клиент из " << ip << endl;


        string clientName = "User" + to_string(clientCounter++);

        thread(HandleClient, clientSock, clientName).detach();
    }

    closesocket(listenSock);
    WSACleanup();
    return 0;
}