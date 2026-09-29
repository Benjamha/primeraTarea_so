#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <poll.h>
#include <random>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>
#include <deque>
#include <fstream>
#include <sstream>
#include <iostream>

namespace {

constexpr int MAX_MSG_LEN = 200;

volatile sig_atomic_t g_sigint_flag = 0;
volatile sig_atomic_t g_sigchld_flag = 0;
int g_self_pipe[2] = {-1, -1};

void signal_handler(int signo) {
    if (signo == SIGINT)
        g_sigint_flag = 1;
    else if (signo == SIGCHLD)
        g_sigchld_flag = 1;

    char c = 1;
    write(g_self_pipe[1], &c, 1);
}

void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");

    if (a == std::string::npos)
        return "";

    size_t b = s.find_last_not_of(" \t\r\n");

    return s.substr(a, b - a + 1);
}

enum class Estado {
    PENDIENTE,
    LISTA,
    EJECUTANDO,
    OK,
    FALLIDA,
    ABORTADA
};

struct Nodo {
    std::string id;
    std::string nombre;
    int tiempo_ms = 0;
    std::vector<std::string> deps;
    std::vector<std::string> hijos;
    int deps_pendientes = 0;
    Estado estado = Estado::PENDIENTE;
    std::vector<std::string> mensajes_entrada;
};

struct Proceso {
    std::string node_id;
    pid_t pid = -1;
    int out_fd = -1;
    std::string out_buffer;
    bool pipe_cerrado = false;
    bool reaped = false;
    bool exito = false;
};

std::unordered_map<std::string, Nodo> g_nodos;
std::vector<std::string> g_orden_declaracion;

std::unordered_map<std::string, Proceso> g_procesos;
std::unordered_map<pid_t, std::string> g_pid_a_nodo;

int g_running = 0;
int g_ok_count = 0;
int g_fail_count = 0;
int g_aborted_count = 0;

bool parsear_plan(const std::string &ruta, std::mt19937 &rng) {
    std::ifstream in(ruta);

    if (!in.is_open()) {
        std::cerr << "Error: no se pudo abrir el archivo '" << ruta << "'\n";
        return false;
    }

    std::uniform_int_distribution<int> dist_tiempo(100, 5000);

    std::string linea;
    int num_linea = 0;

    while (std::getline(in, linea)) {
        num_linea++;

        std::string l = trim(linea);

        if (l.empty() || l[0] == '#')
            continue;

        std::vector<std::string> campos;
        std::stringstream ss(l);
        std::string campo;

        while (std::getline(ss, campo, ':'))
            campos.push_back(campo);

        if (campos.size() < 3) {
            std::cerr << "Error de formato en linea " << num_linea << "\n";
            return false;
        }

        Nodo n;

        n.id = trim(campos[0]);
        n.nombre = trim(campos[1]);

        if (n.id.empty()) {
            std::cerr << "Error en linea " << num_linea << ": ID vacio\n";
            return false;
        }

        if (g_nodos.count(n.id)) {
            std::cerr << "Error en linea " << num_linea
                      << ": ID duplicado '" << n.id << "'\n";
            return false;
        }

        std::string tiempo = trim(campos[2]);

        if (tiempo.empty()) {
            n.tiempo_ms = dist_tiempo(rng);
        } else {
            try {
                n.tiempo_ms = std::stoi(tiempo);
            } catch (...) {
                std::cerr << "Error en linea " << num_linea
                          << ": tiempo invalido\n";
                return false;
            }
        }

        if (campos.size() >= 4) {
            std::stringstream ds(trim(campos[3]));
            std::string dep;

            while (std::getline(ds, dep, ',')) {
                dep = trim(dep);

                if (!dep.empty())
                    n.deps.push_back(dep);
            }
        }

        g_orden_declaracion.push_back(n.id);
        g_nodos[n.id] = n;
    }

    for (const auto &id : g_orden_declaracion) {
        Nodo &n = g_nodos[id];

        for (const auto &dep : n.deps) {
            if (!g_nodos.count(dep)) {
                std::cerr << "Error: la actividad '" << id
                          << "' depende de '" << dep
                          << "' que no existe\n";
                return false;
            }

            g_nodos[dep].hijos.push_back(id);
        }

        n.deps_pendientes = static_cast<int>(n.deps.size());
    }

    return true;
}

[[noreturn]] void child_main(const Nodo &n, int in_fd, int out_fd) {
    signal(SIGINT, SIG_IGN);

    std::string entrada;
    char buffer[256];

    while (true) {
        ssize_t r = read(in_fd, buffer, sizeof(buffer));

        if (r <= 0)
            break;

        entrada.append(buffer, static_cast<size_t>(r));
    }

    close(in_fd);

    usleep(static_cast<useconds_t>(n.tiempo_ms) * 1000);

    bool forzar_falla =
        n.nombre.find("_FALLA") != std::string::npos ||
        n.nombre.find("_falla") != std::string::npos;

    std::mt19937 rng(
        static_cast<unsigned>(getpid()) ^
        static_cast<unsigned>(time(nullptr))
    );

    std::uniform_int_distribution<int> dist(1, 100);

    if (forzar_falla || dist(rng) <= 3) {
        close(out_fd);
        _exit(EXIT_FAILURE);
    }

    std::string msg =
        "[" + n.id + ":" + n.nombre + "] listo en " +
        std::to_string(n.tiempo_ms) + "ms";

    if (msg.size() > MAX_MSG_LEN)
        msg.resize(MAX_MSG_LEN);

    write(out_fd, msg.data(), msg.size());

    close(out_fd);
    _exit(EXIT_SUCCESS);
}

void marcar_abortada(const std::string &id) {
    Nodo &n = g_nodos[id];

    if (n.estado == Estado::ABORTADA ||
        n.estado == Estado::OK ||
        n.estado == Estado::FALLIDA)
        return;

    n.estado = Estado::ABORTADA;
    g_aborted_count++;

    for (const auto &hijo : n.hijos)
        marcar_abortada(hijo);
}

void lanzar_proceso(const std::string &id) {
    Nodo &n = g_nodos[id];

    int in_pipe[2];
    int out_pipe[2];

    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
        std::cerr << "Error creando pipes para '" << id << "'\n";
        n.estado = Estado::FALLIDA;
        g_fail_count++;
        return;
    }

    std::string entrada;

    for (size_t i = 0; i < n.mensajes_entrada.size(); i++) {
        if (i > 0)
            entrada += " | ";

        entrada += n.mensajes_entrada[i];
    }

    pid_t pid = fork();

    if (pid < 0) {
        std::cerr << "Error en fork() para '" << id << "'\n";

        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);

        n.estado = Estado::FALLIDA;
        g_fail_count++;
        return;
    }

    if (pid == 0) {
        for (const auto &p : g_procesos) {
            if (p.second.out_fd >= 0)
                close(p.second.out_fd);
        }

        close(g_self_pipe[0]);
        close(g_self_pipe[1]);

        close(in_pipe[1]);
        close(out_pipe[0]);

        child_main(n, in_pipe[0], out_pipe[1]);
    }

    close(in_pipe[0]);

    if (!entrada.empty())
        write(in_pipe[1], entrada.data(), entrada.size());

    close(in_pipe[1]);

    close(out_pipe[1]);
    set_nonblocking(out_pipe[0]);

    Proceso p;
    p.node_id = id;
    p.pid = pid;
    p.out_fd = out_pipe[0];

    g_procesos[id] = p;
    g_pid_a_nodo[pid] = id;

    n.estado = Estado::EJECUTANDO;
    g_running++;
}

void abortar_todo() {
    std::cerr << "\n[planificador] SIGINT recibido: abortando toda la simulacion...\n";

    for (auto &p : g_procesos) {
        if (!p.second.reaped)
            kill(p.second.pid, SIGTERM);
    }

    for (auto &p : g_procesos) {
        if (p.second.reaped)
            continue;

        int status;
        pid_t r = waitpid(p.second.pid, &status, WNOHANG);

        if (r == 0) {
            usleep(50000);

            r = waitpid(p.second.pid, &status, WNOHANG);

            if (r == 0)
                kill(p.second.pid, SIGKILL);
        }

        waitpid(p.second.pid, &status, 0);
    }

    for (const auto &id : g_orden_declaracion) {
        Nodo &n = g_nodos[id];

        if (n.estado == Estado::PENDIENTE ||
            n.estado == Estado::LISTA ||
            n.estado == Estado::EJECUTANDO) {
            n.estado = Estado::ABORTADA;
            g_aborted_count++;
        }
    }
}

}

int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "Uso: " << argv[0] << " plan.txt K\n";
        return EXIT_FAILURE;
    }

    int K;

    try {
        K = std::stoi(argv[2]);
    } catch (...) {
        std::cerr << "Error: K debe ser un entero\n";
        return EXIT_FAILURE;
    }

    if (K < 1) {
        std::cerr << "Error: K debe ser >= 1\n";
        return EXIT_FAILURE;
    }

    std::mt19937 rng(
        static_cast<unsigned>(time(nullptr)) ^
        static_cast<unsigned>(getpid())
    );

    if (!parsear_plan(argv[1], rng))
        return EXIT_FAILURE;

    std::cerr << "[planificador] "
              << g_orden_declaracion.size()
              << " actividades cargadas, K=" << K << "\n";

    if (pipe(g_self_pipe) != 0) {
        std::cerr << "Error creando self-pipe\n";
        return EXIT_FAILURE;
    }

    set_nonblocking(g_self_pipe[0]);
    set_nonblocking(g_self_pipe[1]);

    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGCHLD, &sa, nullptr);

    std::deque<std::string> listos;

    for (const auto &id : g_orden_declaracion) {
        Nodo &n = g_nodos[id];

        if (n.deps_pendientes == 0) {
            n.estado = Estado::LISTA;
            listos.push_back(id);
        }
    }

    bool abortando = false;

    while (!abortando && (g_running > 0 || !listos.empty())) {

        while (!listos.empty() && g_running < K) {
            std::string id = listos.front();
            listos.pop_front();

            if (g_nodos[id].estado == Estado::LISTA)
                lanzar_proceso(id);
        }

        if (g_running == 0)
            break;

        std::vector<pollfd> fds;
        std::vector<std::string> orden_fds;

        fds.push_back({g_self_pipe[0], POLLIN, 0});

        for (auto &p : g_procesos) {
            if (!p.second.pipe_cerrado) {
                fds.push_back({p.second.out_fd, POLLIN, 0});
                orden_fds.push_back(p.first);
            }
        }

        int ready = poll(fds.data(), fds.size(), -1);

        if (ready < 0) {
            if (errno == EINTR)
                continue;

            std::cerr << "Error en poll()\n";
            break;
        }

        if (fds[0].revents & POLLIN) {
            char buffer[64];

            while (read(g_self_pipe[0], buffer, sizeof(buffer)) > 0) {}
        }

        for (size_t i = 0; i < orden_fds.size(); i++) {
            const std::string &id = orden_fds[i];
            Proceso &p = g_procesos[id];

            if (!(fds[i + 1].revents & (POLLIN | POLLHUP)))
                continue;

            char buffer[256];
            ssize_t r;

            while ((r = read(p.out_fd, buffer, sizeof(buffer))) > 0)
                p.out_buffer.append(buffer, static_cast<size_t>(r));

            if (r == 0) {
                close(p.out_fd);
                p.pipe_cerrado = true;
            }
        }

        if (g_sigchld_flag) {
            g_sigchld_flag = 0;

            int status;
            pid_t pid;

            while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
                auto it = g_pid_a_nodo.find(pid);

                if (it == g_pid_a_nodo.end())
                    continue;

                Proceso &p = g_procesos[it->second];

                p.reaped = true;
                p.exito =
                    WIFEXITED(status) &&
                    WEXITSTATUS(status) == 0;
            }
        }

        std::vector<std::string> terminados;

        for (auto &p : g_procesos) {
            if (p.second.pipe_cerrado &&
                p.second.reaped &&
                g_nodos[p.first].estado == Estado::EJECUTANDO) {
                terminados.push_back(p.first);
            }
        }

        for (const auto &id : terminados) {
            Proceso &p = g_procesos[id];
            Nodo &n = g_nodos[id];

            g_running--;

            if (p.exito) {
                n.estado = Estado::OK;
                g_ok_count++;

                std::cerr << "[OK]  " << id
                          << " (" << n.nombre << ") -> "
                          << p.out_buffer << "\n";

                for (const auto &hijo_id : n.hijos) {
                    Nodo &hijo = g_nodos[hijo_id];

                    if (hijo.estado == Estado::ABORTADA)
                        continue;

                    hijo.mensajes_entrada.push_back(p.out_buffer);
                    hijo.deps_pendientes--;

                    if (hijo.deps_pendientes == 0) {
                        hijo.estado = Estado::LISTA;
                        listos.push_back(hijo_id);
                    }
                }
            } else {
                n.estado = Estado::FALLIDA;
                g_fail_count++;

                std::cerr << "[FAIL] " << id
                          << " (" << n.nombre
                          << ") fallo internamente; se aborta su rama dependiente\n";

                for (const auto &hijo : n.hijos)
                    marcar_abortada(hijo);
            }

            g_pid_a_nodo.erase(p.pid);
            g_procesos.erase(id);
        }

        if (g_sigint_flag) {
            abortando = true;
            abortar_todo();
        }
    }

    if (!abortando) {
        for (const auto &id : g_orden_declaracion) {
            if (g_nodos[id].estado == Estado::PENDIENTE) {
                g_nodos[id].estado = Estado::ABORTADA;
                g_aborted_count++;
            }
        }
    }

    std::cerr << "\n[planificador] Resumen: "
              << g_ok_count << " OK, "
              << g_fail_count << " fallidas, "
              << g_aborted_count << " abortadas, de "
              << g_orden_declaracion.size()
              << " actividades totales.\n";

    return abortando ? 130 : (g_fail_count > 0 ? 1 : 0);
}
