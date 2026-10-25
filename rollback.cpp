// O core do Arduino confirma o firmware sozinho no boot, a não ser que esta
// função diga que a confirmação fica para depois. Aqui fica: o .ino só confirma
// depois de 1 minuto no ar e conectado (confirmFirmware). Até lá, qualquer
// reinício faz o bootloader voltar para a versão anterior.
//
// Fica num .cpp separado porque o extern "C" no .ino confunde o gerador de
// protótipos do Arduino.
extern "C" bool verifyRollbackLater() { return true; }
